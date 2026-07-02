/**
 * Minimal editor-bridge readiness probe for the harness: connect to :55557, ask
 * `mcp_status`, classify. Used to gate editor-dependent tests and by the editor
 * launcher's boot wait.
 */

import { BRIDGE_HOST, BRIDGE_PORT } from "./env.ts";

export async function probeBridge(
  timeoutMs = 2000,
): Promise<"down" | "initializing" | "interactive"> {
  let socket: Awaited<ReturnType<typeof Bun.connect>> | undefined;
  return new Promise((resolve) => {
    let settled = false;
    const chunks: Uint8Array[] = [];
    const done = (s: "down" | "initializing" | "interactive") => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      try {
        socket?.end();
      } catch {
        /* closed */
      }
      resolve(s);
    };
    const timer = setTimeout(() => done(chunks.length ? "initializing" : "down"), timeoutMs);

    Bun.connect({
      hostname: BRIDGE_HOST,
      port: BRIDGE_PORT,
      socket: {
        open(s) {
          s.write(JSON.stringify({ type: "mcp_status", params: {} }));
        },
        data(_s, data) {
          chunks.push(data);
          try {
            const resp = JSON.parse(Buffer.concat(chunks).toString("utf-8"));
            done(resp?.result?.ready === true ? "interactive" : "initializing");
          } catch {
            /* partial */
          }
        },
        error: () => done("down"),
        connectError: () => done("down"),
      },
    })
      .then((s) => (socket = s))
      .catch(() => done("down"));
  });
}

/** True iff a live, interactive editor is reachable right now. */
export async function editorReady(): Promise<boolean> {
  return (await probeBridge()) === "interactive";
}
