// Batch editor-bridge caller — runs a SEQUENCE of compiled C++ commands against the
// UnrealMCP editor bridge (127.0.0.1:55557) in one `bun` invocation, bypassing the
// MCP server's TS tool surface (so it works even when the client MCP session is stale
// after a server restart). Companion to bridge-call.ts (single command).
//
// Usage (three forms):
//   bun scripts/bridge-batch.ts '<json array of {type,params}>'
//   bun scripts/bridge-batch.ts --file ops.json          # array in a file
//   echo '<json array>' | bun scripts/bridge-batch.ts -  # array on stdin
//
// Each op: {"type":"command_name","params":{...}}  OR  {"type":..., "label":"note", ...rest=params}
// Prints a JSON array of {label?, type, ok, status, error?, result?} — one per op — and
// exits non-zero if any op failed (status !== "success"). Stops on the first failure
// unless --keep-going is passed.
//
// Wire format mirrors src/bridge/connection.ts: send JSON {type, params}; read until one
// JSON object parses; one short-lived connection per op (the editor listener loops accept).

const argv = process.argv.slice(2);
const keepGoing = argv.includes("--keep-going");
const quiet = argv.includes("--quiet");
const rest = argv.filter((a) => a !== "--keep-going" && a !== "--quiet");

async function readInput(): Promise<string> {
  if (rest[0] === "--file") {
    if (!rest[1]) { console.error("--file requires a path"); process.exit(2); }
    return await Bun.file(rest[1]).text();
  }
  if (rest[0] === "-" || rest.length === 0) {
    const chunks: Uint8Array[] = [];
    for await (const c of Bun.stdin.stream()) chunks.push(c);
    return Buffer.concat(chunks).toString("utf8");
  }
  return rest[0]!;
}

type Op = { type: string; params?: Record<string, unknown>; label?: string; [k: string]: unknown };

const raw = await readInput();
let ops: Op[];
try {
  const parsed = JSON.parse(raw);
  ops = Array.isArray(parsed) ? parsed : [parsed];
} catch (e) {
  console.error("Failed to parse ops JSON:", (e as Error).message);
  process.exit(2);
}

const PORT = Number(process.env.UNREAL_BRIDGE_PORT ?? 55557);
// Per-op watchdog: if the editor never replies (hung game thread, infinite loop in a
// handler, PIE deadlock), the socket promise would otherwise never resolve and the
// whole script (and any agent running it) would hang forever. Always bound it.
const OP_TIMEOUT_MS = Number(process.env.UNREAL_BRIDGE_TIMEOUT_MS ?? 90000);

function sendOne(type: string, params: Record<string, unknown>): Promise<any> {
  const payload = JSON.stringify({ type, params });
  const chunks: Uint8Array[] = [];
  return new Promise((resolve) => {
    let settled = false;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const settle = (v: any, s?: any) => {
      if (settled) return;
      settled = true;
      if (timer) clearTimeout(timer);
      try { s?.end?.(); } catch {}
      resolve(v);
    };
    const finish = (s?: any) => {
      const text = Buffer.concat(chunks).toString("utf8");
      try { settle(JSON.parse(text), s); } catch { settle({ status: "error", error: "unparseable response", raw: text }, s); }
    };
    timer = setTimeout(() => {
      settle({ status: "error", error: `bridge op timed out after ${OP_TIMEOUT_MS}ms`, error_code: "bridge_timeout", error_hint: "editor did not reply — it may be hung, mid-build, or stuck in PIE; raise UNREAL_BRIDGE_TIMEOUT_MS for known-slow ops" });
    }, OP_TIMEOUT_MS);
    Bun.connect({
      hostname: "127.0.0.1",
      port: PORT,
      socket: {
        open(s) { s.write(payload); },
        data(s, d) {
          chunks.push(d);
          try { JSON.parse(Buffer.concat(chunks).toString("utf8")); finish(s); } catch { /* keep reading */ }
        },
        close() { finish(); },
        error(_s, e) { settle({ status: "error", error: String(e) }); },
        connectError(_s, e) { settle({ status: "error", error: "connect: " + String(e) }); },
      },
    });
  });
}

const out: any[] = [];
let anyFail = false;
for (const op of ops) {
  const { type, label } = op;
  const params = (op.params ?? Object.fromEntries(Object.entries(op).filter(([k]) => !["type", "label", "params"].includes(k)))) as Record<string, unknown>;
  const env = await sendOne(type, params);
  const ok = env?.status === "success";
  if (!ok) anyFail = true;
  out.push({ ...(label ? { label } : {}), type, ok, status: env?.status, ...(ok ? { result: env?.result } : { error: env?.error, error_code: env?.error_code, error_hint: env?.error_hint }) });
  if (!quiet) console.error(`${ok ? "ok  " : "FAIL"} ${label ? "[" + label + "] " : ""}${type}${ok ? "" : "  -> " + (env?.error ?? env?.status)}`);
  if (!ok && !keepGoing) break;
}

console.log(JSON.stringify(out, null, 2));
process.exit(anyFail ? 1 : 0);
