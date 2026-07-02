/**
 * Tier 3 — code-execution mode. Two tools:
 *   - `code_api`   : the typed `unreal.*` listing (read on demand, by domain).
 *   - `code_run`   : execute a TS snippet that drives the editor and returns only
 *                    what it logs/returns — intermediate data stays in the sandbox.
 *
 * Token win: "list 5000 assets, keep the 3 that match" costs ~3 rows of context
 * instead of 5000. Opt-in (the `code` surface mode) — see README security note.
 */

import { z } from "zod";
import type { ToolDef } from "../../registry/types.ts";
import type { ToolRegistry } from "../../registry/index.ts";
import { defineTool } from "../../domains/_shared.ts";
import { generateApi } from "./generate.ts";
import { runCode } from "./sandbox.ts";

export function codeTools(registry: ToolRegistry): ToolDef[] {
  const codeApi = defineTool({
    name: "code_api",
    domain: "code",
    description:
      "List the `unreal.*` functions available inside code_run (optionally one " +
      "domain). Each returns Promise<{status,result,error}>. Get exact params with " +
      "catalog_describe. Read this before writing a code_run snippet.",
    input: z.object({
      domain: z.string().optional().describe("Restrict the listing to one domain (see catalog_domains)."),
    }),
    annotations: { readOnlyHint: true, idempotentHint: true },
    handler: (a) => ({
      status: "success",
      result: { api: generateApi(registry, a.domain) },
    }),
  });

  const codeRun = defineTool({
    name: "code_run",
    domain: "code",
    description:
      "Run a TypeScript snippet that drives the editor via `unreal.<tool>(params)` " +
      "(each awaitable, returns the tool envelope). Do loops/filtering/aggregation " +
      "in code; ONLY your console.log output and the returned value come back — " +
      "large intermediate results never enter context. Top-level await is allowed.",
    input: z.object({
      code: z.string().min(1).describe("TS snippet body. `unreal` and `console` are in scope; you may return a value."),
      timeout_ms: z.number().int().min(100).max(300_000).default(30_000),
    }),
    handler: async (a, ctx) => {
      const r = await runCode(a.code, { registry, ctx, timeoutMs: a.timeout_ms });
      if (r.error) {
        return {
          status: "error",
          error: r.error,
          result: { logs: r.logs, calls: r.calls },
        };
      }
      return {
        status: "success",
        result: { value: r.value, logs: r.logs, calls: r.calls },
      };
    },
  });

  return [codeApi, codeRun];
}
