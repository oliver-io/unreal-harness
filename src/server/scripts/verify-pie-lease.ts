/**
 * Live end-to-end verification of the PIE lease against a real editor.
 *
 * Spawns a SECOND MCP server on a test port (so the canonical :8765 server and
 * any agents on it are untouched), pointed at the same editor bridge, and drives
 * it with TWO independent streamable-HTTP MCP sessions — the only way to exercise
 * the per-session identity + cross-agent coordination that the in-process test
 * harness (InMemoryTransport, no session id) can't.
 *
 * Checks: distinct session ids · real pie_start/stop forward · a second agent is
 * told to wait (pie_busy + queue position + holder) · promotion on release ·
 * disconnect releases the lease · stale-PIE takeover on the next acquire · the
 * keep-alive reconciler is running.
 *
 * SAFE: if the editor is already running PIE (someone else is using it), the
 * PIE-driving steps are skipped — only the non-destructive plumbing is checked.
 * Always tears down PIE + the spawned server in `finally`.
 *
 *   cd src/server && bun scripts/verify-pie-lease.ts
 */

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";
import type { Envelope } from "../src/bridge/envelope.ts";

const PORT = Number(process.env.VERIFY_PIE_PORT ?? 8799);
const URL_MCP = new URL(`http://127.0.0.1:${PORT}/mcp`);
const WATCHDOG_MS = 180_000;

let pass = 0;
let fail = 0;
const check = (name: string, ok: boolean, detail = ""): boolean => {
  console.log(`${ok ? "  PASS" : "  FAIL"}  ${name}${detail ? ` — ${detail}` : ""}`);
  ok ? pass++ : fail++;
  return ok;
};
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

interface Sess {
  client: Client;
  transport: StreamableHTTPClientTransport;
  call(name: string, args?: Record<string, unknown>): Promise<Envelope>;
  inner(env: Envelope): Record<string, unknown>;
  lease(env: Envelope): Record<string, unknown>;
}

async function connect(name: string): Promise<Sess> {
  const transport = new StreamableHTTPClientTransport(URL_MCP);
  const client = new Client({ name: `verify-${name}`, version: "0.0.0" });
  await client.connect(transport);
  const call = async (n: string, args: Record<string, unknown> = {}): Promise<Envelope> => {
    const res = await client.callTool({ name: n, arguments: args });
    const content = (res.content ?? []) as { type: string; text?: string }[];
    return JSON.parse(content.find((c) => c.type === "text")?.text ?? "{}") as Envelope;
  };
  const inner = (env: Envelope) =>
    (env.result && typeof env.result === "object" ? env.result : {}) as Record<string, unknown>;
  const lease = (env: Envelope) =>
    (inner(env).pie_lease ?? {}) as Record<string, unknown>;
  return { client, transport, call, inner, lease };
}

/** Poll a session's pie_get_state until is_running matches, or timeout. */
async function waitRunning(s: Sess, want: boolean, timeoutMs = 25_000): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const st = s.inner(await s.call("pie_get_state"));
    if (Boolean(st.is_running) === want) return true;
    await sleep(500);
  }
  return false;
}

async function main(): Promise<void> {
  console.log(`\n=== PIE lease live verification (test server :${PORT}) ===\n`);

  // ── spawn the new-code server on a test port ────────────────────────────
  const proc = Bun.spawn(["bun", "src/main.ts"], {
    // Inherits cwd — run this script from src/server (bun scripts/verify-pie-lease.ts).
    env: {
      ...process.env,
      UNREAL_MCP_PORT: String(PORT),
      UNREAL_MCP_PIE_ACQUIRE_CAP_MS: "1500", // short waits in the test
      UNREAL_MCP_PIE_LIVENESS_POLL_MS: "2000",
    },
    stdout: "pipe",
    stderr: "pipe",
  });
  let serverLog = "";
  const drain = async (stream: ReadableStream<Uint8Array>) => {
    const dec = new TextDecoder();
    for await (const chunk of stream) serverLog += dec.decode(chunk);
  };
  void drain(proc.stdout);
  void drain(proc.stderr);

  const watchdog = setTimeout(() => {
    console.error("WATCHDOG: verification exceeded time budget — killing.");
    try { proc.kill(); } catch {}
    process.exit(2);
  }, WATCHDOG_MS);

  let a: Sess | undefined;
  let b: Sess | undefined;
  let c: Sess | undefined;
  try {
    // wait until the server accepts an MCP session
    const bootDeadline = Date.now() + 20_000;
    for (;;) {
      try {
        a = await connect("A");
        break;
      } catch {
        if (Date.now() > bootDeadline) throw new Error("test server did not boot");
        await sleep(300);
      }
    }
    b = await connect("B");

    // ── 1. distinct session identity (the critical plumbing) ──────────────
    const sidA = a!.transport.sessionId;
    const sidB = b!.transport.sessionId;
    check("two clients get distinct, defined session ids", !!sidA && !!sidB && sidA !== sidB,
      `A=${sidA?.slice(0, 8)} B=${sidB?.slice(0, 8)}`);

    // ── editor readiness + PIE idle guard ─────────────────────────────────
    const ready = (a!.inner(await a!.call("mcp_status")) as { ready?: boolean }).ready === true;
    check("editor reachable + ready", ready);
    const st0 = a!.inner(await a!.call("pie_get_state"));
    check("pie_get_state surfaces a pie_lease block", !!st0.pie_lease,
      JSON.stringify(st0.pie_lease ?? null).slice(0, 120));

    if (!ready) {
      console.log("\n  editor not ready — skipping PIE-driving checks.\n");
    } else if (st0.is_running === true) {
      console.log("\n  PIE already running (another agent?) — skipping PIE-driving checks to avoid disruption.\n");
    } else {
      // ── 2. A acquires and PIE actually starts ───────────────────────────
      const startA = await a!.call("pie_start", { requester: "verify-A" });
      check("A pie_start succeeds", startA.status === "success", a!.lease(startA).state as string);
      check("A pie_start reports you_hold", a!.lease(startA).you_hold === true);
      check("A PIE reaches is_running=true", await waitRunning(a!, true));

      // ── 3. B is told to wait, with a queue position + the holder ─────────
      const startB = await b!.call("pie_start", { requester: "verify-B" });
      check("B pie_start refused while A holds", startB.status === "error" && startB.error_code === "pie_busy",
        `code=${startB.error_code}`);
      check("B is queued at position 1", b!.lease(startB).position === 1,
        `position=${b!.lease(startB).position}`);
      const holderLbl = (b!.lease(startB).holder as { label?: string } | null)?.label;
      check("B sees A as the holder", holderLbl === "verify-A", `holder=${holderLbl}`);
      const stB = b!.inner(await b!.call("pie_get_state"));
      check("B pie_get_state: you_hold=false, your_position=1",
        (stB.pie_lease as Record<string, unknown>).you_hold === false &&
        (stB.pie_lease as Record<string, unknown>).your_position === 1);

      // ── 4. A releases → B is promoted ───────────────────────────────────
      const stopA = await a!.call("pie_stop");
      check("A pie_stop releases the lease", stopA.status === "success", a!.lease(stopA).state as string);
      await waitRunning(a!, false);
      const startB2 = await b!.call("pie_start", { requester: "verify-B" });
      check("B promoted: pie_start now succeeds", startB2.status === "success", b!.lease(startB2).state as string);
      check("B PIE reaches is_running=true", await waitRunning(b!, true));

      // ── 5. disconnect releases the lease; next acquire takes over the orphan
      // terminateSession() sends the session DELETE — the graceful-disconnect
      // signal (a dying client's dropped streams hit the same server onclose).
      await b!.transport.terminateSession();
      await b!.client.close(); // B vanishes WITHOUT pie_stop — PIE still live
      await sleep(2000); // let onSessionClosed fire
      c = await connect("C");
      const stC0 = c!.inner(await c!.call("pie_get_state"));
      check("disconnected holder's lease was released",
        (stC0.pie_lease as Record<string, unknown>).holder === null,
        `holder=${JSON.stringify((stC0.pie_lease as Record<string, unknown>).holder)}`);
      check("editor still has the orphaned PIE running (pre-takeover)", stC0.is_running === true);
      const startC = await c!.call("pie_start", { requester: "verify-C" });
      check("C acquires the freed lease", startC.status === "success", c!.lease(startC).state as string);
      check("C takeover yields a running PIE", await waitRunning(c!, true));

      // ── 6. cleanup stop ─────────────────────────────────────────────────
      const stopC = await c!.call("pie_stop");
      check("C pie_stop releases the lease", stopC.status === "success");
      check("PIE fully stopped", await waitRunning(c!, false));
    }

    // ── 7. keep-alive reconciler is running ───────────────────────────────
    check("keep-alive reconciler started", /PIE lease reconciler started/.test(serverLog));
  } finally {
    clearTimeout(watchdog);
    // Best-effort: make sure we never leave PIE running on the live editor.
    for (const s of [a, b, c]) {
      try { await s?.call("pie_stop"); } catch {}
      try { await s?.client.close(); } catch {}
    }
    try { proc.kill(); } catch {}
  }

  console.log(`\n=== ${pass} passed, ${fail} failed ===\n`);
  if (fail > 0 && serverLog) {
    console.log("---- spawned server log (tail) ----");
    console.log(serverLog.split("\n").slice(-25).join("\n"));
  }
  process.exit(fail > 0 ? 1 : 0);
}

main().catch((e) => {
  console.error("verification crashed:", e);
  process.exit(3);
});
