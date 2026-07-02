/**
 * Unit tests for the build lock (cross-agent build serialization). Pure logic:
 * an injected clock fast-forwards past the TTL and an injected PID-liveness fn
 * simulates a crashed/alive build process — no real processes or waits.
 */

import { expect, test, describe, beforeEach, afterAll } from "bun:test";
import { config } from "../src/config.ts";
import {
  acquire,
  release,
  heartbeat,
  status,
  buildInProgress,
  __test,
} from "../src/build/lock.ts";

let t = 5_000_000;
let alivePids: Set<number>;

beforeEach(() => {
  __test.reset();
  t = 5_000_000;
  alivePids = new Set([100, 200, 300]);
  __test.setClock(() => t);
  __test.setPidAlive((pid) => alivePids.has(pid));
});
afterAll(() => __test.reset());

const TTL = config.buildLockTtlMs;

describe("build lock", () => {
  test("first builder acquires; a different builder is refused", () => {
    const a = acquire({ pid: 100, label: "agentA", target: "SampleEditor" });
    expect(a.outcome).toBe("acquired");
    expect(a.fresh).toBe(true);
    expect(status().in_progress).toBe(true);

    const b = acquire({ pid: 200, label: "agentB" });
    expect(b.outcome).toBe("busy");
    expect(b.build.label).toBe("agentA"); // told who holds it
    expect(b.build.pid).toBe(100);
  });

  test("same builder re-acquiring refreshes (idempotent), not busy", () => {
    acquire({ pid: 100, label: "agentA" });
    t += TTL / 2;
    const again = acquire({ pid: 100, label: "agentA", target: "SampleEditor" });
    expect(again.outcome).toBe("acquired");
    expect(again.fresh).toBe(false);
    expect(again.build.expires_at).toBe(t + TTL); // TTL pushed out
  });

  test("release frees the lock for the next builder", () => {
    const a = acquire({ pid: 100, label: "agentA" });
    expect(release({ buildId: a.build.build_id }).outcome).toBe("released");
    expect(status().in_progress).toBe(false);
    expect(acquire({ pid: 200, label: "agentB" }).outcome).toBe("acquired");
  });

  test("release by a non-holder is rejected; release when free is a no-op", () => {
    acquire({ pid: 100, label: "agentA" });
    expect(release({ pid: 999 }).outcome).toBe("not_holder");
    expect(status().in_progress).toBe(true); // still held
    release({ pid: 100 });
    expect(release({ pid: 100 }).outcome).toBe("already_free");
  });

  test("a crashed build (dead PID) is reclaimed the moment anyone touches the lock", () => {
    acquire({ pid: 100, label: "agentA" });
    alivePids.delete(100); // build process dies without releasing
    // Next builder's acquire reaps the dead holder and succeeds.
    const b = acquire({ pid: 200, label: "agentB" });
    expect(b.outcome).toBe("acquired");
    expect(b.build.pid).toBe(200);
  });

  test("buildInProgress reflects PID liveness and feeds the editor-down message", () => {
    acquire({ pid: 100, label: "agentA", target: "SampleEditor" });
    expect(buildInProgress()?.pid_alive).toBe(true);
    alivePids.delete(100);
    expect(buildInProgress()).toBeNull(); // reaped on read
  });

  test("a hung-but-alive build is reclaimed only after the TTL", () => {
    acquire({ pid: 100, label: "agentA" }); // PID stays alive (hung compiler)
    expect(status().in_progress).toBe(true);
    t += TTL + 1;
    // PID still 'alive', but TTL elapsed → reclaimed.
    const b = acquire({ pid: 200, label: "agentB" });
    expect(b.outcome).toBe("acquired");
  });

  test("heartbeat extends the holder's TTL", () => {
    acquire({ pid: 100, label: "agentA" });
    t += TTL - 1000;
    const hb = heartbeat({ pid: 100 });
    expect(hb.ok).toBe(true);
    expect(hb.build?.expires_at).toBe(t + TTL);
    // A non-holder heartbeat fails.
    expect(heartbeat({ pid: 999 }).ok).toBe(false);
  });
});
