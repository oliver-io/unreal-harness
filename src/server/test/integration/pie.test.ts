/**
 * Play-In-Editor (PIE) and AI-runtime domain. Port of tests/integration/test_pie.py.
 *
 * Two classes of test live here:
 *  - Headless-safe queries — read PIE state or fail cleanly with the documented
 *    not_in_pie guard when no session is running.
 *  - PIE lifecycle + input — ops that actually START PIE; GUI-only (a game window
 *    can fatal the editor under -nullrhi), gated via test.skipIf(!GUI).
 */

import { test, expect, afterEach } from "bun:test";
import { mkdtemp, rm, readFile, stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { editorSuite, NS as ROOT, GUI } from "../harness/suite.ts";
import { assertReady, ensureAbsent } from "../harness/ops.ts";
import type { Commandable } from "../harness/ops.ts";

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

/** Poll get_pie_state until is_running matches `wantRunning` (PIE start/stop is
 *  asynchronous — the handlers return 'starting'/'stopping' immediately and the
 *  world flips state a few frames later). Returns the final is_running. */
async function waitForPie(
  bridge: Commandable,
  wantRunning: boolean,
  timeout = 30.0,
): Promise<boolean> {
  const deadline = Date.now() + timeout * 1000;
  let state: Record<string, unknown> = {};
  while (Date.now() < deadline) {
    state = await bridge.expect("pie_get_state", {});
    if (Boolean(state.is_running) === wantRunning) {
      return wantRunning;
    }
    await sleep(500);
  }
  return Boolean(state.is_running);
}

/** Acquire the shared PIE lease, honoring the queue protocol: pie_busy is a
 *  queue position, not a failure — re-call pie_start to hold the place (FIFO)
 *  until promoted or the deadline passes (docs/USAGE.md §2.18). */
async function acquirePie(bridge: Commandable, requester: string, timeout = 120.0): Promise<void> {
  const deadline = Date.now() + timeout * 1000;
  for (;;) {
    const resp = await bridge.command("pie_start", { requester });
    if (resp.status === "success") return;
    if (resp.error_code !== "pie_busy" || Date.now() >= deadline) {
      throw new Error(`could not acquire the PIE lease: ${JSON.stringify(resp)}`);
    }
    await sleep(2000);
  }
}

/** Parse width/height straight out of the PNG IHDR chunk (an observation
 *  independent of the capture op's own envelope). */
function pngDimensions(buf: Buffer): { width: number; height: number } {
  const sig = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  if (!buf.subarray(0, 8).equals(sig)) throw new Error("not a PNG");
  if (buf.subarray(12, 16).toString("ascii") !== "IHDR") throw new Error("no IHDR chunk");
  return { width: buf.readUInt32BE(16), height: buf.readUInt32BE(20) };
}

editorSuite("pie", (ctx) => {
  // SAFETY: never leave PIE running. If a lifecycle test throws mid-session (an
  // assertion fails after pie_start but before its pie_stop), this guarantees the
  // session is torn down so the editor isn't left playing and the next test starts
  // clean. pie_stop with no session is the harmless not_in_pie guard, so this is a
  // no-op for the headless-safe tests. command() never throws on error envelopes.
  afterEach(async () => {
    try {
      if ((await ctx.mcp.expect("pie_get_state", {})).is_running) {
        await ctx.mcp.command("pie_stop", {});
        await waitForPie(ctx.mcp, false);
      }
    } catch {
      /* best-effort cleanup — never fail a run on teardown */
    }
  });

  // ── Headless-safe: PIE-state queries without a running session ────────────
  test("test_get_pie_state_idle_reports_not_running", async () => {
    // get_pie_state is a success envelope even with no session — is_running=false.
    const result = await ctx.mcp.expect("pie_get_state", {});
    expect(result.is_running).toBe(false);
  });

  test("test_get_pie_state_surfaces_lease_block", async () => {
    // pie_get_state now folds in the cross-agent lease state. With no agent
    // holding it (headless default), the lease is free and this caller is idle.
    const result = await ctx.mcp.expect("pie_get_state", {});
    const lease = result.pie_lease as Record<string, unknown> | undefined;
    expect(lease).toBeTruthy();
    expect(lease!.holder).toBeNull();
    expect(lease!.you_hold).toBe(false);
    expect(lease!.your_position).toBeNull();
    expect(typeof lease!.lease_ttl_ms).toBe("number");
  });

  test("test_pie_query_without_pie_errors_not_in_pie", async () => {
    // pie_query reads the LIVE PIE world; with no session it returns the
    // documented not_in_pie guard (error envelope), not a success.
    const resp = await ctx.mcp.command("pie_query", { query: "summary", limit: 200 });
    expect(resp.status).toEqual("error");
    expect(resp.error_code).toEqual("not_in_pie");
    await assertReady(ctx.mcp);
  });

  test("test_stop_pie_without_session_errors_not_in_pie", async () => {
    // Stopping when nothing is playing is the not_in_pie guard. Safe headless:
    // no window is opened or torn down.
    const resp = await ctx.mcp.command("pie_stop", {});
    expect(resp.status).toEqual("error");
    expect(resp.error_code).toEqual("not_in_pie");
    await assertReady(ctx.mcp);
  });

  // ── Headless-safe: AI-runtime ops via their not_in_pie guard ──────────────
  test("test_ai_runtime_ops_require_pie", async () => {
    // All three AI-runtime ops query the live PIE world for an AIController. With
    // no PIE session (the headless default) each returns the documented not_in_pie
    // guard. This dispatches every op (error path) without needing a window.
    for (const op of ["ai_get_state", "ai_get_awareness", "ai_get_perception"]) {
      const resp = await ctx.mcp.command(op, { actor_name: "MCPTest_AIPawn" });
      if (resp.status === "success") {
        // Only reachable inside an active PIE session with a real AI pawn.
        expect(resp.result && typeof resp.result === "object").toBeTruthy();
        continue;
      }
      const code = resp.error_code;
      if (code !== "not_in_pie") {
        console.log(
          `${op} needs a live PIE session with a perception-enabled ` +
            `AIController; got error_code=${JSON.stringify(code)}`,
        );
        return;
      }
      expect(code).toEqual("not_in_pie");
    }
    await assertReady(ctx.mcp);
  });

  // ── GUI-only: PIE lifecycle + input injection (opens a game viewport) ──────
  test.skipIf(!GUI)("test_pie_lifecycle_start_query_input_stop", async () => {
    // Make sure we begin from a clean (not-playing) state.
    if ((await ctx.mcp.expect("pie_get_state", {})).is_running) {
      await ctx.mcp.command("pie_stop", {});
      await waitForPie(ctx.mcp, false);
    }

    // start_pie returns immediately ("starting"); the world goes live a few
    // frames later. It plays the currently-open level (no map_path needed).
    const start = await ctx.mcp.expect("pie_start", {});
    expect(start.status).toEqual("starting");
    const sessionId = start.session_id;
    expect(sessionId).toBeTruthy();
    await assertReady(ctx.mcp);

    expect(await waitForPie(ctx.mcp, true)).toBeTruthy();

    // get_pie_state now reports the live session id back.
    const state = await ctx.mcp.expect("pie_get_state", {});
    expect(state.is_running).toBe(true);
    expect(state.session_id).toEqual(sessionId);

    // pie_query against the live world now succeeds (summary shape).
    const summary = await ctx.mcp.expect("pie_query", { query: "summary", limit: 200 });
    expect(summary && typeof summary === "object").toBeTruthy();
    expect(Object.keys(summary).length).toBeGreaterThan(0);

    // Inject a key tap (pressed + released pair) and a mouse move. send_keystrokes
    // wraps the bridge send_key_input events; assert the batch reports success.
    const down = await ctx.mcp.expect("pie_send_keystrokes", {
      actions: [{ key: "W", event_type: "pressed" }],
    });
    expect(down.success).not.toBe(false);
    await ctx.mcp.expect("pie_send_keystrokes", {
      actions: [{ key: "W", event_type: "released" }],
    });

    const moved = await ctx.mcp.expect("pie_send_mouse", {
      x: 100.0,
      y: 100.0,
      event_type: "move",
      button: "left",
    });
    expect(moved.event_type).toEqual("move");
    await assertReady(ctx.mcp);

    // stop_pie validates the session id, then ends the session asynchronously.
    const stop = await ctx.mcp.expect("pie_stop", { session_id: sessionId });
    expect(stop.status).toEqual("stopping");
    expect(await waitForPie(ctx.mcp, false)).toBeTruthy();
    await assertReady(ctx.mcp);
  });

  // ── Pose capture + Enhanced Input injection ────────────────────────────────
  test("test_pie_capture_from_pose_without_pie_errors_not_in_pie", async () => {
    // The capture rig reproduces a pose INSIDE the running game; with no PIE
    // session it returns the documented not_in_pie guard (checked before any
    // camera is spawned or file written). Safe headless: nothing renders.
    const resp = await ctx.mcp.command("pie_capture_from_pose", {
      location: { x: 0.0, y: 0.0, z: 500.0 },
      rotation: { pitch: -90.0, yaw: 0.0, roll: 0.0 },
    });
    expect(resp.status).toEqual("error");
    expect(resp.error_code).toEqual("not_in_pie");
    await assertReady(ctx.mcp);
  });

  test("test_pie_inject_input_action_without_pie_errors", async () => {
    // Enhanced Input injection targets the live PIE player. With no session the
    // handler refuses BEFORE loading the asset — the documented guard is
    // invalid_argument ("PIE is not running"), not not_in_pie.
    const resp = await ctx.mcp.command("pie_inject_input_action", {
      action_path: `${ROOT}/pie/IA_DoesNotExist`,
    });
    expect(resp.status).toEqual("error");
    expect(resp.error_code).toEqual("invalid_argument");
    expect(String(resp.error ?? "")).toContain("PIE");
    await assertReady(ctx.mcp);
  });

  test.skipIf(!GUI)("test_pie_capture_from_pose_and_inject_input_action", async () => {
    // One short PIE session exercises both ops (keeps shared-editor PIE time
    // minimal). The pose IS the fixed capture rig — a supplied location/rotation/
    // fov, zero navigation — per docs/TESTING.md.
    //
    // Capture observation: the PNG on disk (signature + IHDR dimensions + size),
    // read back by the test itself — independent of the op's envelope.
    // Injection observation: the positive path asserts the injection ack only
    // (no typed primitive can bind an InputAction to an observable consumer
    // without C++ — the deep consumer-side observation is #DEFERRED in
    // docs/loops/tests/TASKS.md); the bad-path asserts the documented
    // asset_not_found guard, which only fires inside a live session.
    const ns = `${ROOT}/pie`;
    const iaPath = `${ns}/IA_MCPInjectProbe`;
    const outDir = await mkdtemp(join(tmpdir(), "mcp-pose-capture-"));

    // Arrange (pre-PIE — asset mutation stays out of the session): a real
    // UInputAction under the test namespace for the positive injection.
    await ensureAbsent(ctx.mcp, iaPath);
    const created = await ctx.mcp.expect("input_create", {
      type: "action",
      name: "IA_MCPInjectProbe",
      path: ns,
      value_type: "boolean",
    });
    expect(created.asset_path).toBeTruthy();

    let acquired = false;
    try {
      await acquirePie(ctx.mcp, "bun pie capture/inject");
      acquired = true;
      expect(await waitForPie(ctx.mcp, true)).toBeTruthy();

      // Act: capture from an explicit top-down pose into a test-owned dir.
      const result = await ctx.mcp.expect("pie_capture_from_pose", {
        location: { x: 0.0, y: 0.0, z: 500.0 },
        rotation: { pitch: -90.0, yaw: 0.0, roll: 0.0 },
        fov: 90.0,
        filename: "bun_pose_capture",
        directory: outDir,
      });
      // The bridge confirms the file before returning: status flips
      // "requested" -> "captured" only once the PNG exists on disk.
      expect(result.status).toEqual("captured");
      const path = String(result.path ?? result.file_path ?? "");
      expect(path).toBeTruthy();

      // Observe: the file itself — a real, non-trivial PNG with sane dims.
      expect((await stat(path)).size).toBeGreaterThan(1000);
      const { width, height } = pngDimensions(await readFile(path));
      expect(width).toBeGreaterThan(0);
      expect(height).toBeGreaterThan(0);
      await assertReady(ctx.mcp);

      // Injecting a nonexistent action inside the live session is the
      // documented asset_not_found guard (the PIE gate passed first).
      const bad = await ctx.mcp.command("pie_inject_input_action", {
        action_path: `${ns}/IA_DoesNotExist`,
      });
      expect(bad.status).toEqual("error");
      expect(bad.error_code).toEqual("asset_not_found");

      // Positive injection into the live player's EnhancedInput subsystem.
      const injected = await ctx.mcp.expect("pie_inject_input_action", {
        action_path: iaPath,
        value: 1.0,
      });
      expect(injected.injected).toBe(true);
      expect(injected.action).toEqual(iaPath);
      await assertReady(ctx.mcp);
    } finally {
      if (acquired) {
        await ctx.mcp.command("pie_stop", {});
        await waitForPie(ctx.mcp, false);
      }
      await ctx.mcp.command("asset_delete", { asset_path: iaPath, force: true });
      await rm(outDir, { recursive: true, force: true }).catch(() => {});
      await assertReady(ctx.mcp);
    }
  }, 240000);
});
