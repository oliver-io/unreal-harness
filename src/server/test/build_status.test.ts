/**
 * End-to-end test for the `build_status` TOOL (not just the lock module —
 * that is `build_lock.test.ts`). Act: invoke the `build_status` handler pulled
 * from the canonical registry (`buildRegistry()`), the same ToolDef an MCP
 * client's call is dispatched to. Arrange/observe the REAL build-lock store
 * (`src/build/lock.ts` module state — no mocks of the lock) via its public
 * `acquire`/`release`, holding the lock with THIS test process's live PID so
 * the lazy PID reaper cannot reclaim it mid-test.
 *
 * Editor probe: the handler also probes editor liveness via
 * `ctx.conn.sendCommand("mcp_status")`. We must not touch the live shared
 * editor from a unit test, so the BRIDGE TRANSPORT (only) is stubbed with a
 * deterministic error envelope — exactly what the real connection returns when
 * the editor is down (`sendCommand` never throws). That makes the
 * editor-unreachable → `{reachable:false, ready:false}` mapping assertable
 * without depending on (or disturbing) any live editor another agent may own.
 *
 * Leak safety: the build lock here is IN-MEMORY state of this bun test
 * process — not the running MCP server's lock and not an on-disk file — so a
 * leaked acquisition cannot block other agents' real builds. Teardown still
 * releases + resets in afterEach (runs even on assertion failure) so state
 * never bleeds into other test files sharing this process (build_lock.test.ts
 * injects a fake clock/PID-checker into the same module).
 */

import { expect, test, describe, beforeEach, afterEach } from "bun:test";
import { buildRegistry } from "../src/register.ts";
import { acquire, release, __test } from "../src/build/lock.ts";
import type { ToolContext, ToolDef } from "../src/registry/types.ts";
import { covers } from "./harness/coverage.ts";

// Stub of the bridge transport only (never the lock store): a non-success
// envelope, i.e. what the real BridgeConnection yields for a down editor.
const ctx = {
  conn: {
    sendCommand: async () => ({
      status: "error",
      error: "editor unreachable (test stub)",
    }),
  },
} as unknown as ToolContext;

function buildStatusTool(): ToolDef {
  const def = buildRegistry().get("build_status");
  if (!def) throw new Error("build_status not registered in the canonical registry");
  return def;
}

// Real clock + real PID liveness for this file (a sibling test file injects
// fakes into the same module state).
beforeEach(() => __test.reset());
afterEach(() => {
  // Belt and braces: release by our own PID (no-op if already free), then
  // reset injectables. Runs even when an assertion above failed.
  release({ pid: process.pid });
  __test.reset();
});

describe("build_status tool", () => {
  covers("build_status");

  test("no lock held: build.in_progress is false with a null holder", async () => {
    const r: any = await buildStatusTool().handler({}, ctx);
    expect(r.status).toBe("success");
    expect(r.result.build.in_progress).toBe(false);
    expect(r.result.build.holder).toBeNull();
    // Editor probe failed (stubbed transport) → documented unreachable shape.
    expect(r.result.editor).toEqual({ reachable: false, ready: false });
  });

  test("held lock: in_progress is true and the holder is surfaced; release clears it", async () => {
    // Live PID: the lock's lazy reaper checks process liveness on every read,
    // so holding with our own PID keeps the lock genuinely held.
    const a = acquire({
      pid: process.pid,
      label: "build_status.test",
      target: "SampleEditor",
    });
    expect(a.outcome).toBe("acquired");

    const tool = buildStatusTool();
    const held: any = await tool.handler({}, ctx);
    expect(held.status).toBe("success");
    expect(held.result.build.in_progress).toBe(true);
    // Fields the tool description promises: holder {label,pid,target,started,pid_alive}.
    const holder = held.result.build.holder;
    expect(holder.pid).toBe(process.pid);
    expect(holder.label).toBe("build_status.test");
    expect(holder.target).toBe("SampleEditor");
    expect(holder.pid_alive).toBe(true); // real PID check against our live process
    expect(typeof holder.started_at).toBe("number");
    // Editor unreachable + build in progress is the tool's "wait and retry" state.
    expect(held.result.editor.reachable).toBe(false);

    expect(release({ buildId: a.build.build_id }).outcome).toBe("released");

    const after: any = await tool.handler({}, ctx);
    expect(after.status).toBe("success");
    expect(after.result.build.in_progress).toBe(false);
    expect(after.result.build.holder).toBeNull();
  });
});
