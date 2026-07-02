/**
 * Main-thread half of code-mode: spawn the Worker, service its `unreal.*` RPC
 * calls by dispatching through the registry (same validate→handler→envelope path
 * as any tool call), enforce a wall-clock timeout, and collect logs + return
 * value.
 */

import type { ToolRegistry } from "../../registry/index.ts";
import type { ToolContext } from "../../registry/types.ts";
import { applyAliases } from "../../registry/aliases.ts";

export interface CodeRunResult {
  logs: string[];
  value?: unknown;
  error?: string;
  calls: number;
}

export function runCode(
  code: string,
  deps: { registry: ToolRegistry; ctx: ToolContext; timeoutMs?: number },
): Promise<CodeRunResult> {
  const timeoutMs = deps.timeoutMs ?? 30_000;
  const worker = new Worker(new URL("./worker.ts", import.meta.url).href);
  let calls = 0;

  return new Promise<CodeRunResult>((resolve) => {
    let settled = false;
    const finish = (r: CodeRunResult) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      worker.terminate();
      resolve(r);
    };
    const timer = setTimeout(
      () => finish({ logs: [], error: `code-mode timed out after ${timeoutMs}ms`, calls }),
      timeoutMs,
    );

    worker.onmessage = async (e: MessageEvent) => {
      const msg = e.data as
        | { type: "call"; id: number; name: string; params: Record<string, unknown> }
        | { type: "done"; logs: string[]; value?: unknown; error?: string };

      if (msg.type === "call") {
        calls++;
        const out = await dispatch(deps.registry, deps.ctx, msg.name, msg.params);
        worker.postMessage({ type: "result", id: msg.id, value: out.value, error: out.error });
        return;
      }
      if (msg.type === "done") {
        finish({ logs: msg.logs, value: msg.value, error: msg.error, calls });
      }
    };

    worker.onerror = (e: ErrorEvent) => finish({ logs: [], error: e.message, calls });
    worker.postMessage({ type: "run", code });
  });
}

async function dispatch(
  registry: ToolRegistry,
  ctx: ToolContext,
  name: string,
  params: Record<string, unknown>,
): Promise<{ value?: unknown; error?: string }> {
  const def = registry.get(name);
  if (!def || def.domain === "catalog" || def.domain === "code") {
    return { error: `Unknown or non-callable tool: ${name}` };
  }
  const parsed = def.input.safeParse(params ?? {});
  if (!parsed.success) {
    return { error: `Invalid params for ${name}: ${parsed.error.message}` };
  }
  try {
    return {
      value: await def.handler(
        applyAliases(parsed.data as Record<string, unknown>, def.aliases),
        ctx,
      ),
    };
  } catch (err) {
    return { error: err instanceof Error ? err.message : String(err) };
  }
}
