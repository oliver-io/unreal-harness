// Direct editor-bridge caller — invokes ANY compiled C++ command on the UnrealMCP
// editor bridge (127.0.0.1:55557), bypassing the MCP server's TS tool surface.
// Useful for commands that are compiled into the plugin but not yet surfaced by a
// running server (which would need a restart + client reconnect to pick up).
//
//   cd src/server && bun scripts/bridge-call.ts <command> '<paramsJSON>'
//   bun scripts/bridge-call.ts level_save_as '{"package_path":"/Game/Untitled"}'
//
// Wire format mirrors src/bridge/connection.ts: send JSON {type, params}; read until
// one JSON object parses.
const command = process.argv[2];
const params = process.argv[3] ? JSON.parse(process.argv[3]) : {};
if (!command) { console.error("usage: bridge-call.ts <command> '<paramsJSON>'"); process.exit(2); }

const payload = JSON.stringify({ type: command, params });
const chunks: Uint8Array[] = [];
// Per-op watchdog so a hung/non-responsive editor can never hang this script forever.
const OP_TIMEOUT_MS = Number(process.env.UNREAL_BRIDGE_TIMEOUT_MS ?? 90000);
let timedOut = false;

await new Promise<void>((resolve) => {
  let settled = false;
  let timer: ReturnType<typeof setTimeout> | undefined;
  const finish = (s?: any) => { if (settled) return; settled = true; if (timer) clearTimeout(timer); try { s?.end?.(); } catch {} resolve(); };
  timer = setTimeout(() => { timedOut = true; console.error(`bridge op timed out after ${OP_TIMEOUT_MS}ms`); finish(); }, OP_TIMEOUT_MS);
  Bun.connect({
    hostname: "127.0.0.1",
    port: Number(process.env.UNREAL_BRIDGE_PORT ?? 55557),
    socket: {
      open(s) { s.write(payload); },
      data(s, d) {
        chunks.push(d);
        try { JSON.parse(Buffer.concat(chunks).toString("utf8")); finish(s); } catch { /* keep reading */ }
      },
      close() { finish(); },
      error(_s, e) { console.error("bridge error:", e); finish(); },
      connectError(_s, e) { console.error("connect error:", e); finish(); },
    },
  });
});

const text = Buffer.concat(chunks).toString("utf8");
console.log(text || "(no response)");
if (timedOut || !text) process.exit(1);
