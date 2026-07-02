/**
 * Code-mode Worker. Runs an agent-authored snippet with an injected `unreal`
 * object whose every method is an async RPC to the main thread (which owns the
 * editor connection + registry). `console.log` is captured. The snippet's
 * awaited return value and logs are posted back; everything else (intermediate
 * arrays, large reads) stays here and never reaches the model.
 *
 * NOTE: this isolates crashes and enforces a timeout (via main-thread
 * terminate). It is NOT a security sandbox — see codemode/run tool docs.
 */

declare const self: Worker;

let seq = 0;
const pending = new Map<number, { resolve: (v: unknown) => void; reject: (e: Error) => void }>();

const unreal = new Proxy(
  {},
  {
    get(_t, name: string) {
      return (params: Record<string, unknown> = {}) =>
        new Promise((resolve, reject) => {
          const id = ++seq;
          pending.set(id, { resolve, reject });
          self.postMessage({ type: "call", id, name, params });
        });
    },
  },
);

self.onmessage = async (e: MessageEvent) => {
  const msg = e.data as
    | { type: "result"; id: number; value?: unknown; error?: string }
    | { type: "run"; code: string };

  if (msg.type === "result") {
    const p = pending.get(msg.id);
    if (!p) return;
    pending.delete(msg.id);
    if (msg.error) p.reject(new Error(msg.error));
    else p.resolve(msg.value);
    return;
  }

  if (msg.type === "run") {
    const logs: string[] = [];
    const fmt = (a: unknown[]) =>
      a.map((x) => (typeof x === "string" ? x : safeJson(x))).join(" ");
    const sandboxConsole = {
      log: (...a: unknown[]) => logs.push(fmt(a)),
      error: (...a: unknown[]) => logs.push(fmt(a)),
      warn: (...a: unknown[]) => logs.push(fmt(a)),
      info: (...a: unknown[]) => logs.push(fmt(a)),
    };
    try {
      // eslint-disable-next-line no-new-func
      const fn = new Function(
        "unreal",
        "console",
        `"use strict"; return (async () => {\n${msg.code}\n})();`,
      );
      const value = await fn(unreal, sandboxConsole);
      self.postMessage({ type: "done", logs, value: safeClone(value) });
    } catch (err) {
      self.postMessage({ type: "done", logs, error: err instanceof Error ? err.message : String(err) });
    }
  }
};

function safeJson(x: unknown): string {
  try {
    return JSON.stringify(x);
  } catch {
    return String(x);
  }
}

/** Strip anything non-structured-cloneable from the return value. */
function safeClone(v: unknown): unknown {
  try {
    return JSON.parse(JSON.stringify(v ?? null));
  } catch {
    return String(v);
  }
}
