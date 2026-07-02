/**
 * TEMPLATE — standing PASS/FAIL end-to-end verify script (the guarded-loop oracle).
 *
 * Copy to `src/server/scripts/verify-<game>.ts`. Connects to the running MCP server
 * as a REAL MCP client, drives a self-playing scenario in PIE, asserts on log
 * markers + pie_query positions, screenshots, stops PIE, prints PASS/FAIL, exits
 * non-zero on failure (CI-friendly). Model: src/server/scripts/verify-template.ts.
 *
 * Prereq: the editor AND the MCP server are already running. Run from the server dir:
 *   cd src/server && bun scripts/verify-<game>.ts
 *
 * Two non-negotiable safety rails (this drives a LIVE editor + PIE):
 *   (1) a per-call timeout so a stuck tool can't block forever;
 *   (2) a hard wall-clock watchdog that force-stops PIE and exits non-zero;
 *   plus a `finally` that guarantees pie_stop no matter how we exit.
 */
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

const MCP_URL = new URL(process.env.UNREAL_MCP_URL ?? "http://127.0.0.1:8765/mcp");
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const CALL_TIMEOUT_MS = Number(process.env.VERIFY_CALL_TIMEOUT_MS ?? 60000);
const HARD_CAP_MS = Number(process.env.VERIFY_TIMEOUT_MS ?? 120000);

const client = new Client({ name: "feature-verify", version: "1.0.0" }, { capabilities: {} });
await client.connect(new StreamableHTTPClientTransport(MCP_URL));

/** callTool → unwrap the envelope JSON; raced against a timeout so a hung editor
 *  can't stall the run. Returns an error envelope instead of throwing. */
async function call(name: string, args: Record<string, unknown> = {}): Promise<any> {
  try {
    const r: any = await Promise.race([
      client.callTool({ name, arguments: args }),
      sleep(CALL_TIMEOUT_MS).then(() => { throw new Error(`call '${name}' timed out`); }),
    ]);
    const text = r?.content?.[0]?.text ?? "{}";
    try { return JSON.parse(text); } catch { return { raw: text }; }
  } catch (e: any) {
    return { status: "error", error: e?.message ?? String(e) };
  }
}

let finished = false;
const watchdog = setTimeout(async () => {
  if (finished) return;
  console.error(`\n!! verify timed out after ${HARD_CAP_MS}ms — force-stopping PIE and aborting.`);
  try { await call("pie_stop"); } catch {}
  try { await client.close(); } catch {}
  process.exit(2);
}, HARD_CAP_MS);
watchdog.unref?.();

const ok = (env: any) => env?.status === "success";
const results: string[] = [];
let pass = true;
const check = (cond: boolean, label: string, detail = "") => {
  results.push(`${cond ? "PASS" : "FAIL"}  ${label}${detail ? " — " + detail : ""}`);
  if (!cond) pass = false;
};

try {
  // 1) Editor ready (boot gate) — poll, don't assume.
  const status = await call("mcp_status");
  check(status?.result?.ready === true, "editor ready");

  // 2) Clean slate, then start the self-playing scenario.
  await call("pie_stop").catch(() => {});
  await sleep(1500);
  check(ok(await call("pie_start")), "pie_start");
  await sleep(4000); // settle: BeginPlay + placed director + spawns

  // 3) OBSERVE a self-driving subject moving (positions over time).
  const posOf = async (pat: string) => {
    const q = await call("pie_query", { query: "actors", filter: pat });
    return (q?.result?.actors ?? [])[0]?.location ?? null;
  };
  const p1 = await posOf("REPLACE_actor");
  await sleep(4000);
  const p2 = await posOf("REPLACE_actor");
  const dist = p1 && p2 ? Math.hypot(p2.x - p1.x, p2.y - p1.y, p2.z - p1.z) : 0;
  check(dist > 1000, "subject moves", `Δ=${dist.toFixed(0)}u`);

  // 4) OBSERVE log markers — the primary oracle. Instrument the feature to emit a
  //    stable `[FEATURE] event=...` prefix, then assert on it.
  await sleep(8000);
  const logs = await call("editor_read_logs", { grep: "[FEATURE]", tail: 200 });
  const lines: string[] = logs?.lines ?? logs?.result?.lines ?? [];
  check(lines.some((l) => /\[FEATURE\] event=success/i.test(l)), "feature emitted success marker",
        `${lines.length} [FEATURE] lines`);

  // 5) Evidence screenshot (for a human / vision check — not a machine assertion).
  check(ok(await call("editor_screenshot", { filename: "verify_feature.png", mode: "viewport" })),
        "screenshot captured");

  await call("pie_stop");
} catch (e: any) {
  check(false, "harness exception", e?.message ?? String(e));
} finally {
  finished = true;
  clearTimeout(watchdog);
  try { await call("pie_stop"); } catch {} // guarantee PIE is stopped however we exit
  console.log("\n==== FEATURE verification ====");
  for (const r of results) console.log("  " + r);
  console.log(`==== ${pass ? "PASS" : "FAIL"} ====\n`);
  await client.close();
  process.exit(pass ? 0 : 1);
}
