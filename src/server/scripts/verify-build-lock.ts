/**
 * Live verification of the build lock. Spawns a test-port MCP server pointed at a
 * BOGUS bridge port (so the editor reads as unreachable — no real editor touched),
 * then exercises: the /build REST endpoints (acquire/busy/release/status), PID
 * liveness reclaim, the editor-down contextualization (an editor tool call during
 * a build explains itself), build_status, and the REAL build-editor.ps1 refusing
 * to build while the lock is held.
 *
 *   cd src/server && bun scripts/verify-build-lock.ts
 */

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";
import type { Envelope } from "../src/bridge/envelope.ts";

const PORT = Number(process.env.VERIFY_BUILD_PORT ?? 8798);
const BASE = `http://127.0.0.1:${PORT}`;
const WATCHDOG_MS = 90_000;

let pass = 0;
let fail = 0;
const check = (name: string, ok: boolean, detail = ""): boolean => {
  console.log(`${ok ? "  PASS" : "  FAIL"}  ${name}${detail ? ` — ${detail}` : ""}`);
  ok ? pass++ : fail++;
  return ok;
};
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function rest(method: string, path: string, body?: unknown): Promise<any> {
  const r = await fetch(`${BASE}${path}`, {
    method,
    headers: { "content-type": "application/json" },
    body: body ? JSON.stringify(body) : undefined,
  });
  return r.json();
}

async function main(): Promise<void> {
  console.log(`\n=== Build lock live verification (test server :${PORT}, bogus bridge) ===\n`);

  const proc = Bun.spawn(["bun", "src/main.ts"], {
    env: {
      ...process.env,
      UNREAL_MCP_PORT: String(PORT),
      UNREAL_BRIDGE_PORT: "59997", // nothing listens here → editor "unreachable"
    },
    stdout: "pipe",
    stderr: "pipe",
  });
  let serverLog = "";
  void (async () => {
    const dec = new TextDecoder();
    for await (const c of proc.stderr) serverLog += dec.decode(c);
  })();

  const watchdog = setTimeout(() => {
    console.error("WATCHDOG: exceeded time budget — killing.");
    try { proc.kill(); } catch {}
    process.exit(2);
  }, WATCHDOG_MS);

  let child: ReturnType<typeof Bun.spawn> | undefined;
  try {
    // wait for the server to accept HTTP
    const boot = Date.now() + 15_000;
    for (;;) {
      try {
        const s = await rest("GET", "/build/status");
        if (s) break;
      } catch {
        if (Date.now() > boot) throw new Error("test server did not boot");
        await sleep(300);
      }
    }

    // ── 1. REST acquire / busy / release ──────────────────────────────────
    const free0 = await rest("GET", "/build/status");
    check("starts with no build in progress", free0.in_progress === false);

    const acq = await rest("POST", "/build/acquire", {
      pid: process.pid,
      target: "SampleEditor Win64 Development",
      label: "verify-A",
    });
    check("first acquire succeeds", acq.ok === true && acq.status === "acquired",
      `build_id=${acq.build?.build_id?.slice(0, 8)}`);

    const busy = await rest("POST", "/build/acquire", {
      pid: proc.pid, // a different, alive process
      label: "verify-B",
    });
    check("second builder is refused (busy)", busy.ok === false && busy.status === "busy");
    check("busy response names the holder", busy.holder?.label === "verify-A" && busy.holder?.pid === process.pid,
      `holder=${busy.holder?.label}/${busy.holder?.pid}`);

    const st1 = await rest("GET", "/build/status");
    check("status shows holder with pid_alive=true", st1.in_progress === true && st1.holder?.pid_alive === true);

    const rel = await rest("POST", "/build/release", { build_id: acq.build.build_id });
    check("release frees the lock", rel.ok === true && rel.status === "released");
    check("status free after release", (await rest("GET", "/build/status")).in_progress === false);

    // ── 2. PID liveness reclaim (crashed build) ───────────────────────────
    child = Bun.spawn(["bun", "-e", "setTimeout(() => {}, 60000)"]);
    await sleep(300);
    await rest("POST", "/build/acquire", { pid: child.pid, label: "verify-crashy" });
    check("a build held by a live child is in progress", (await rest("GET", "/build/status")).in_progress === true);
    child.kill();
    await sleep(800); // let the OS reap the child
    const afterKill = await rest("GET", "/build/status");
    check("crashed build (dead PID) is reclaimed on next status read", afterKill.in_progress === false);

    // ── 3. editor-down contextualization + build_status MCP tool ──────────
    await rest("POST", "/build/acquire", { pid: process.pid, target: "SampleEditor", label: "verify-A" });
    const transport = new StreamableHTTPClientTransport(new URL(`${BASE}/mcp`));
    const client = new Client({ name: "verify-build", version: "0.0.0" });
    await client.connect(transport);
    const call = async (n: string, a: Record<string, unknown> = {}): Promise<Envelope> => {
      const res = await client.callTool({ name: n, arguments: a });
      const content = (res.content ?? []) as { type: string; text?: string }[];
      return JSON.parse(content.find((c) => c.type === "text")?.text ?? "{}") as Envelope;
    };

    const editorCall = await call("actor_query", { filter: "" });
    check("editor tool during a build returns a build-aware error",
      editorCall.status === "error" && /build is in progress/i.test(editorCall.error ?? ""),
      `code=${editorCall.error_code}`);

    const bs = await call("build_status");
    const bsr = (bs.result ?? {}) as any;
    check("build_status: build in progress + editor unreachable",
      bsr.build?.in_progress === true && bsr.editor?.reachable === false,
      `build=${bsr.build?.in_progress} editor.reachable=${bsr.editor?.reachable}`);
    await client.close();

    // ── 4. the REAL build-editor.ps1 refuses while the lock is held ───────
    // Lock still held by this process (verify-A). Run build-editor.ps1 → it must
    // refuse and exit 75 BEFORE invoking Build.bat.
    const ps1 = new URL("../../../scripts/build-editor.ps1", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1");
    const pwsh = Bun.spawn(
      ["pwsh", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps1],
      { env: { ...process.env, UNREAL_MCP_PORT: String(PORT) }, stdout: "pipe", stderr: "pipe" },
    );
    await pwsh.exited;
    const psOut = (await new Response(pwsh.stdout).text()) + (await new Response(pwsh.stderr).text());
    check("build-editor.ps1 refuses while a build is in progress (exit 75)", pwsh.exitCode === 75,
      `exit=${pwsh.exitCode}`);
    check("build-editor.ps1 prints the 'can't build right now' message",
      /can't build right now/i.test(psOut));

    await rest("POST", "/build/release", { pid: process.pid });
  } finally {
    clearTimeout(watchdog);
    try { child?.kill(); } catch {}
    try { await rest("POST", "/build/release", { pid: process.pid }); } catch {}
    try { proc.kill(); } catch {}
  }

  console.log(`\n=== ${pass} passed, ${fail} failed ===\n`);
  if (fail > 0) console.log("---- server log (tail) ----\n" + serverLog.split("\n").slice(-20).join("\n"));
  process.exit(fail > 0 ? 1 : 0);
}

main().catch((e) => {
  console.error("verification crashed:", e);
  process.exit(3);
});
