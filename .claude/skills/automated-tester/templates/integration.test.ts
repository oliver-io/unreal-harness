/**
 * TEMPLATE — Bun in-process integration test (the `/automated-tester` default).
 *
 * Copy to `src/server/test/integration/<feature>.test.ts`, rename the suite, and
 * fill the Arrange → Act → Observe → Assert beats. Delete the parts you don't use.
 *
 *   cd src/server && bun test test/integration/<feature>.test.ts
 *
 * `editorSuite` SKIPS the whole block when no interactive editor is on :55557, so
 * `bun test` stays green on a dev box with no engine and runs real round-trips when
 * an editor is up. ctx.mcp = in-process MCP client (TOOL names, validated);
 * ctx.bridge = raw bridge client (WIRE names, unvalidated). Mirror this as a
 * `@covers` pytest test in tests/integration/ to keep the coverage oracle green.
 */

import { test, expect, beforeAll, afterEach } from "bun:test";
import { editorSuite, NS, GUI } from "../harness/suite.ts";
import { ensureAbsent, assertReady } from "../harness/ops.ts";
import type { Commandable } from "../harness/ops.ts";

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

editorSuite("FEATURE", (ctx) => {
  // ── ARRANGE: shared, idempotent, namespaced setup ────────────────────────────
  const SAMPLE = `${NS}/BP_FeatureSample`;
  beforeAll(async () => {
    await ensureAbsent(ctx.mcp, SAMPLE); // re-runnable: delete before create
    await ctx.mcp.expect("bp_create_blueprint", { name: SAMPLE, parent_class: "Actor" });
  });

  // ── Tier 1: static / asset — runs headless (-nullrhi) ────────────────────────
  test("does the observable thing", async () => {
    // ACT — the single operation under test.
    await ctx.mcp.expect("REPLACE_mutator", { /* params */ });

    // OBSERVE — read back through a DIFFERENT, deeper primitive than the mutator.
    const got = await ctx.mcp.expect("REPLACE_reader", { /* params */ });

    // ASSERT — on observed state, not the mutator's echo.
    expect(got /* .field */).toBe(/* expected */);
    await assertReady(ctx.mcp); // crash guard after a risky op
  });

  // ── Dry-run pre-flight (for mutators): preview, prove no side effect ──────────
  test("dry_run previews without mutating", async () => {
    const res = await ctx.mcp.expect("REPLACE_mutator", { /* params */, dry_run: true });
    expect(res.dry_run).toBe(true);
    expect(res.diff).toBeDefined();
    // then read back and confirm nothing actually changed.
  });

  // ── Tier 2: runtime / PIE — GUI-gated if it opens a game viewport ─────────────
  // SAFETY: never leave PIE running, even if an assertion throws mid-session.
  afterEach(async () => {
    try {
      if ((await ctx.mcp.expect("pie_get_state", {})).is_running) {
        await ctx.mcp.command("pie_stop", {}); // command() never throws on error envelope
        await waitForPie(ctx.mcp, false);
      }
    } catch { /* best-effort teardown — never fail a run here */ }
  });

  test.skipIf(!GUI)("runtime behaviour, observed via log markers", async () => {
    const start = await ctx.mcp.expect("pie_start", {}); // async: returns "starting"
    expect(start.status).toEqual("starting");
    expect(await waitForPie(ctx.mcp, true)).toBeTruthy(); // POLL, never sleep-to-wait

    // Drive a SELF-DRIVING subject (bot / auto-tick BP / placed director), then
    // observe — log markers are the primary oracle; pie_query positions corroborate.
    await sleep(3000);
    const logs = await ctx.mcp.expect("editor_read_logs", { grep: "[FEATURE]", tail: 200 });
    const lines: string[] = (logs.lines as string[]) ?? [];
    expect(lines.some((l) => /\[FEATURE\] event=did_thing/.test(l))).toBe(true);

    await ctx.mcp.expect("pie_stop", { session_id: start.session_id });
    expect(await waitForPie(ctx.mcp, false)).toBeTruthy();
    await assertReady(ctx.mcp);
  });
});

/** Poll pie_get_state to a deadline (PIE start/stop flips a few frames after the
 *  handler returns). Returns the final is_running. */
async function waitForPie(bridge: Commandable, want: boolean, timeoutS = 30): Promise<boolean> {
  const deadline = Date.now() + timeoutS * 1000;
  let running = false;
  while (Date.now() < deadline) {
    running = Boolean((await bridge.expect("pie_get_state", {})).is_running);
    if (running === want) return want;
    await sleep(500);
  }
  return running;
}
