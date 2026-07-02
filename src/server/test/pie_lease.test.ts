/**
 * Unit tests for the PIE lease (cross-agent coordination). Pure logic — no editor
 * and no real waits: an injected clock fast-forwards past the TTL, and a 0ms
 * acquire cap makes the "waiting" path resolve immediately.
 */

import { expect, test, describe, beforeEach, afterAll } from "bun:test";
import { config } from "../src/config.ts";
import {
  acquire,
  release,
  markStarted,
  noteLiveness,
  isLeaseHeld,
  snapshot,
  wasEvicted,
  onSessionClosed,
  __test,
} from "../src/pie/lease.ts";

let t = 1_000_000;
const fresh = new AbortController().signal; // never aborts
const aborted = AbortSignal.abort();

beforeEach(() => {
  __test.reset();
  t = 1_000_000;
  __test.setClock(() => t);
});
afterAll(() => __test.resetClock());

const TTL = config.pieLeaseTtlMs;

describe("pie lease", () => {
  test("first agent acquires immediately (fresh)", async () => {
    const r = await acquire("A", "agentA", fresh, 0);
    expect(r.outcome).toBe("acquired");
    expect(r.fresh).toBe(true);
    expect(r.holder?.session_id).toBe("A");
    expect(snapshot("A").you_hold).toBe(true);
  });

  test("second agent waits with a queue position", async () => {
    await acquire("A", "agentA", fresh, 0);
    const r = await acquire("B", "agentB", fresh, 0);
    expect(r.outcome).toBe("waiting");
    expect(r.fresh).toBe(false);
    expect(r.position).toBe(1);
    expect(r.holder?.session_id).toBe("A");
    expect(snapshot("B").your_position).toBe(1);
  });

  test("re-acquire by the holder is a no-op that refreshes the TTL", async () => {
    await acquire("A", "agentA", fresh, 0);
    t += TTL / 2;
    const r = await acquire("A", "agentA", fresh, 0);
    expect(r.outcome).toBe("already_held");
    expect(r.fresh).toBe(false);
    // TTL refreshed from the new now().
    expect(r.holder?.expires_at).toBe(t + TTL);
  });

  test("release promotes the next waiter in FIFO order", async () => {
    await acquire("A", "agentA", fresh, 0);
    await acquire("B", "agentB", fresh, 0); // queue: [B]
    await acquire("C", "agentC", fresh, 0); // queue: [B, C]
    expect(snapshot("C").your_position).toBe(2);

    expect(release("A").outcome).toBe("released");
    // B is at the front now — it can claim; C still waits behind it.
    const rb = await acquire("B", "agentB", fresh, 0);
    expect(rb.outcome).toBe("acquired");
    const rc = await acquire("C", "agentC", fresh, 0);
    expect(rc.outcome).toBe("waiting");
    expect(rc.position).toBe(1);
  });

  test("re-calling while queued keeps your place (no cutting)", async () => {
    await acquire("A", "agentA", fresh, 0);
    await acquire("B", "agentB", fresh, 0); // B position 1
    await acquire("C", "agentC", fresh, 0); // C position 2
    // B re-calls (retry) — must NOT drop behind C.
    const rb = await acquire("B", "agentB", fresh, 0);
    expect(rb.position).toBe(1);
    expect(snapshot("C").your_position).toBe(2);
  });

  test("a stuck holder is reaped after the TTL and the next agent promoted", async () => {
    await acquire("A", "agentA", fresh, 0);
    await acquire("B", "agentB", fresh, 0); // B waits
    t += TTL + 1; // A's lease expires
    const rb = await acquire("B", "agentB", fresh, 0);
    expect(rb.outcome).toBe("acquired");
    expect(rb.holder?.session_id).toBe("B");
    // A was evicted; its pie_stop must be rejected, not forwarded.
    expect(wasEvicted("A")).toBe(true);
    expect(release("A").outcome).toBe("evicted");
    // ...and the eviction record is consumed once reported.
    expect(wasEvicted("A")).toBe(false);
  });

  test("release by a non-holder reports not_holder", () => {
    expect(release("nobody").outcome).toBe("not_holder");
  });

  test("an aborted request never enters/leaves a slot dangling", async () => {
    await acquire("A", "agentA", fresh, 0);
    const r = await acquire("B", "agentB", aborted, 0);
    expect(r.outcome).toBe("aborted");
    expect(snapshot("B").your_position).toBeNull();
    expect(snapshot().queue_length).toBe(0);
  });

  test("a disconnected holder's lease is released for the next agent", async () => {
    await acquire("A", "agentA", fresh, 0);
    await acquire("B", "agentB", fresh, 0);
    onSessionClosed("A"); // holder disconnects
    const rb = await acquire("B", "agentB", fresh, 0);
    expect(rb.outcome).toBe("acquired");
  });

  test("a disconnected waiter is dropped from the queue", async () => {
    await acquire("A", "agentA", fresh, 0);
    await acquire("B", "agentB", fresh, 0);
    await acquire("C", "agentC", fresh, 0);
    onSessionClosed("B"); // middle waiter disconnects
    expect(snapshot("B").your_position).toBeNull();
    expect(snapshot("C").your_position).toBe(1); // C moves up
  });

  // ── keep-alive liveness reconciliation ──────────────────────────────────
  describe("liveness (keep-alive)", () => {
    const STARTUP = config.pieStartupGraceMs;
    const DOWN = config.pieEditorDownGraceMs;

    test("a confirmed-running PIE that stops releases the lease early (crash)", async () => {
      await acquire("A", "agentA", fresh, 0);
      markStarted("A");
      expect(noteLiveness("running").released).toBe(false); // PIE came up
      const v = noteLiveness("stopped"); // ...then died
      expect(v.released).toBe(true);
      expect(v.reason).toBe("pie_ended");
      expect(isLeaseHeld()).toBe(false);
      expect(release("A")).toMatchObject({ outcome: "evicted", reason: "pie_ended" });
    });

    test("PIE that never comes up is released only after the startup grace", async () => {
      await acquire("A", "agentA", fresh, 0);
      markStarted("A");
      expect(noteLiveness("stopped").released).toBe(false); // still warming up
      t += STARTUP + 1;
      const v = noteLiveness("stopped");
      expect(v.released).toBe(true);
      expect(v.reason).toBe("pie_failed_start");
    });

    test("PIE state before markStarted is ignored (takeover/startup churn)", async () => {
      await acquire("A", "agentA", fresh, 0);
      // No markStarted yet: the handler is still doing takeover. A 'stopped' here
      // (a dying prior session) must NOT release the freshly-acquired holder.
      t += STARTUP + 1;
      expect(noteLiveness("stopped").released).toBe(false);
      expect(isLeaseHeld()).toBe(true);
    });

    test("editor unreachable releases only after the down grace", async () => {
      await acquire("A", "agentA", fresh, 0);
      markStarted("A");
      noteLiveness("running");
      expect(noteLiveness("unreachable").released).toBe(false); // transient
      t += DOWN + 1;
      const v = noteLiveness("unreachable");
      expect(v.released).toBe(true);
      expect(v.reason).toBe("editor_down");
    });

    test("a brief unreachable blip does not release once the editor returns", async () => {
      await acquire("A", "agentA", fresh, 0);
      markStarted("A");
      noteLiveness("running");
      t += DOWN - 1;
      noteLiveness("unreachable"); // not yet over the grace
      noteLiveness("running"); // editor came back → down clock resets
      t += DOWN - 1;
      expect(noteLiveness("unreachable").released).toBe(false);
      expect(isLeaseHeld()).toBe(true);
    });

    test("a still-running holder keeps the lease (TTL stays the backstop)", async () => {
      await acquire("A", "agentA", fresh, 0);
      markStarted("A");
      for (let i = 0; i < 5; i++) {
        t += 60_000;
        if (t - 1_000_000 < TTL) expect(noteLiveness("running").released).toBe(false);
      }
      expect(isLeaseHeld()).toBe(true);
    });
  });

  test("long-poll resolves in real time when the holder releases", async () => {
    __test.resetClock(); // use real timers for this one
    await acquire("A", "agentA", fresh, 0);
    const waiting = acquire("B", "agentB", fresh, 5_000); // long-poll
    // Release shortly after; B should be promoted well before its 5s cap.
    setTimeout(() => release("A"), 20);
    const start = Date.now();
    const rb = await waiting;
    expect(rb.outcome).toBe("acquired");
    expect(Date.now() - start).toBeLessThan(2_000);
  });
});
