/**
 * PIE lease — single-editor coordination across many agents.
 *
 * Multiple agents (each its own MCP session) share ONE editor and ONE PIE world.
 * Concurrent PIE is unsupported by the engine, so a slow/forgetful agent stalls
 * everyone else. This module is the data layer that serializes access: a single
 * lease (the "lock") plus a FIFO queue of waiters, keyed by MCP `sessionId`.
 *
 * Contract:
 *   - Exactly one **holder** at a time. Others **queue** in arrival order.
 *   - A held lease has a TTL (`config.pieLeaseTtlMs`, default 10 min). Past it the
 *     holder is considered STUCK: the lease is reaped and the next waiter promoted.
 *     The reaped holder is marked **evicted** so its next `pie_stop`/`pie_start`
 *     is told it lost the lease (it must NOT stop the new holder's PIE).
 *   - Identity is the MCP `sessionId`. When a session disconnects, its lease is
 *     released / its queue slot removed ({@link onSessionClosed}); a reconnect is
 *     a NEW session → back of the line (FIFO).
 *   - State is in-memory ONLY. A server restart wipes the lease + queue and
 *     everyone re-queues from scratch — exactly the intended reset behaviour.
 *
 * This file holds NO bridge/editor logic: it answers "who may use PIE right now",
 * nothing more. The pie domain handler reconciles the live editor (stop a stale
 * PIE before a promoted agent starts) on top of the verdicts here.
 */

import { config } from "../config.ts";
import { log } from "../log.ts";

/** Injectable clock so tests can fast-forward past the TTL without real waits. */
let clock: () => number = () => Date.now();
const now = () => clock();

interface Holder {
  sessionId: string;
  label: string;
  acquiredAt: number;
  expiresAt: number;
  /**
   * When the handler confirmed it issued `pie_start` for THIS holder (after any
   * stale-PIE takeover). Liveness is only evaluated once this is set — before it,
   * an observed "stopped"/"running" reflects the takeover/startup churn, not this
   * holder's session.
   */
  startedAt?: number;
  /** First time the editor was observed running PIE after {@link startedAt}. */
  confirmedRunningAt?: number;
  /** First time the editor was observed unreachable while this holder held it. */
  unreachableSince?: number;
}

interface Waiter {
  sessionId: string;
  label: string;
  enqueuedAt: number;
}

/** Why a holder lost the lease without calling pie_stop — drives the reply. */
export type EvictReason =
  | "expired" // held past the TTL (stuck but possibly still running)
  | "pie_ended" // PIE was confirmed up, then stopped/crashed
  | "pie_failed_start" // PIE never came up within the startup grace
  | "editor_down"; // editor became unreachable past the down grace

/** What the editor reports about PIE, fed to {@link noteLiveness}. */
export type LivenessState = "running" | "stopped" | "unreachable";

// ── module-level singleton state (shared by every session in this process) ──
let holder: Holder | null = null;
const queue: Waiter[] = [];
/** sessionId → why/when its lease was reaped (for the "you were evicted" reply). */
const evicted = new Map<string, { at: number; reason: EvictReason }>();
/** Wake callbacks for in-flight long-polling acquires; fired on any state change. */
const wakeups = new Set<() => void>();

// ── public projections ──────────────────────────────────────────────────

export interface PublicHolder {
  session_id: string;
  label: string;
  acquired_at: number;
  expires_at: number;
  held_ms: number;
  expires_in_ms: number;
}

export interface QueueEntryView {
  session_id: string;
  label: string;
  enqueued_at: number;
  position: number;
}

export interface LeaseSnapshot {
  holder: PublicHolder | null;
  queue: QueueEntryView[];
  queue_length: number;
  lease_ttl_ms: number;
  /** This caller's 1-based queue position, or null if not waiting. */
  your_position: number | null;
  /** True if this caller currently holds the lease. */
  you_hold: boolean;
}

export type AcquireOutcome = "acquired" | "already_held" | "waiting" | "aborted";

export interface AcquireResult {
  outcome: AcquireOutcome;
  /**
   * True when this call NEWLY took the lease — the handler must reconcile the
   * editor (stop any stale PIE) and start a fresh PIE. False for `already_held`
   * (no-op), `waiting`, and `aborted`.
   */
  fresh: boolean;
  position: number | null;
  holder: PublicHolder | null;
  queue_length: number;
  lease_ttl_ms: number;
}

export type ReleaseOutcome = "released" | "not_holder" | "evicted";

export interface ReleaseResult {
  outcome: ReleaseOutcome;
  holder: PublicHolder | null;
  /** Why the caller lost the lease, when `outcome === "evicted"`. */
  reason?: EvictReason;
}

// ── internals ─────────────────────────────────────────────────────────────

function publicHolder(): PublicHolder | null {
  if (!holder) return null;
  const t = now();
  return {
    session_id: holder.sessionId,
    label: holder.label,
    acquired_at: holder.acquiredAt,
    expires_at: holder.expiresAt,
    held_ms: t - holder.acquiredAt,
    expires_in_ms: holder.expiresAt - t,
  };
}

function notify(): void {
  for (const wake of [...wakeups]) wake();
}

/** Release the current holder, recording why, and wake any waiters. */
function evictHolder(reason: EvictReason): { released: true; reason: EvictReason } {
  const h = holder!;
  evicted.set(h.sessionId, { at: now(), reason });
  log.warn(
    `PIE lease released for session ${short(h.sessionId)} (${h.label}): ${reason} ` +
      `(held ${Math.round((now() - h.acquiredAt) / 1000)}s)`,
  );
  holder = null;
  notify();
  return { released: true, reason };
}

/** Drop a stuck holder whose TTL elapsed, and prune stale evicted records. */
function reapExpired(): void {
  if (holder && now() >= holder.expiresAt) evictHolder("expired");
  // Bound the evicted map: forget records older than one TTL window.
  const cutoff = now() - config.pieLeaseTtlMs;
  for (const [sid, rec] of evicted) if (rec.at < cutoff) evicted.delete(sid);
}

function removeFromQueue(sessionId: string): boolean {
  const i = queue.findIndex((w) => w.sessionId === sessionId);
  if (i === -1) return false;
  queue.splice(i, 1);
  return true;
}

function positionOf(sessionId: string): number | null {
  const i = queue.findIndex((w) => w.sessionId === sessionId);
  return i === -1 ? null : i + 1;
}

function enqueue(sessionId: string, label: string): void {
  // Dedupe: a re-call from an already-queued session KEEPS its place (FIFO is by
  // first arrival, not by retry). Only refresh the human label.
  const existing = queue.find((w) => w.sessionId === sessionId);
  if (existing) {
    existing.label = label;
    return;
  }
  queue.push({ sessionId, label, enqueuedAt: now() });
}

/** Claim the lease for `sessionId` iff it is free and the caller is at the front. */
function tryClaim(sessionId: string, label: string): boolean {
  reapExpired();
  if (holder) return false;
  if (queue.length > 0 && queue[0]!.sessionId !== sessionId) return false;
  removeFromQueue(sessionId);
  evicted.delete(sessionId);
  holder = {
    sessionId,
    label,
    acquiredAt: now(),
    expiresAt: now() + config.pieLeaseTtlMs,
  };
  notify();
  return true;
}

/** Resolve on the next state change, after `maxMs`, or when `signal` aborts. */
function waitForChange(maxMs: number, signal?: AbortSignal): Promise<void> {
  return new Promise<void>((resolve) => {
    // Cap each wait at 1s so the loop re-checks TTL expiry even with no event.
    const wait = Math.max(0, Math.min(maxMs, 1000));
    let done = false;
    const finish = () => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      signal?.removeEventListener("abort", finish);
      wakeups.delete(finish);
      resolve();
    };
    const timer = setTimeout(finish, wait);
    signal?.addEventListener("abort", finish, { once: true });
    wakeups.add(finish);
  });
}

const short = (sid: string) => sid.slice(0, 8);

// ── public API ──────────────────────────────────────────────────────────

/**
 * Acquire the PIE lease for `sessionId`, long-polling up to `capMs`.
 *
 *   - free / caller at front  → `acquired` (fresh: true) — handler starts PIE.
 *   - caller already holds it  → `already_held` (fresh: false) — refreshes TTL.
 *   - held by another, cap hit → `waiting` — caller re-calls to keep its slot.
 *   - request aborted/closed   → `aborted` — slot removed.
 */
export async function acquire(
  sessionId: string,
  label: string,
  signal: AbortSignal | undefined,
  capMs: number,
): Promise<AcquireResult> {
  reapExpired();

  // Already the holder: re-affirm and bump the TTL (a no-op re-call).
  if (holder?.sessionId === sessionId) {
    holder.expiresAt = now() + config.pieLeaseTtlMs;
    holder.label = label;
    return result("already_held", false);
  }

  // Fast path: nothing held and nobody waiting → take it outright.
  if (!holder && queue.length === 0 && tryClaim(sessionId, label)) {
    return result("acquired", true);
  }

  enqueue(sessionId, label);
  const deadline = now() + capMs;

  for (;;) {
    if (signal?.aborted) {
      removeFromQueue(sessionId);
      notify();
      return result("aborted", false);
    }
    if (tryClaim(sessionId, label)) return result("acquired", true);

    const remaining = deadline - now();
    if (remaining <= 0) return result("waiting", false);
    await waitForChange(remaining, signal);
  }

  function result(outcome: AcquireOutcome, fresh: boolean): AcquireResult {
    return {
      outcome,
      fresh,
      position: positionOf(sessionId),
      holder: publicHolder(),
      queue_length: queue.length,
      lease_ttl_ms: config.pieLeaseTtlMs,
    };
  }
}

/**
 * Release the lease held by `sessionId` and promote the next waiter.
 *   - `released`   — caller was the holder (handler stops PIE).
 *   - `evicted`    — caller's lease had already expired and was reassigned; it
 *                    must NOT stop the new holder's PIE.
 *   - `not_holder` — caller never held the lease.
 */
export function release(sessionId: string): ReleaseResult {
  reapExpired();
  if (holder?.sessionId === sessionId) {
    log.info(`PIE lease released by session ${short(sessionId)}`);
    holder = null;
    notify();
    return { outcome: "released", holder: publicHolder() };
  }
  const evictRec = evicted.get(sessionId);
  if (evictRec) {
    evicted.delete(sessionId);
    return { outcome: "evicted", holder: publicHolder(), reason: evictRec.reason };
  }
  return { outcome: "not_holder", holder: publicHolder() };
}

/**
 * The handler confirms it has issued `pie_start` for the current holder (after any
 * stale-PIE takeover). Starts the liveness clock — before this, an observed PIE
 * state belongs to the takeover/startup churn, not this holder.
 */
export function markStarted(sessionId: string): void {
  if (holder?.sessionId === sessionId) holder.startedAt ??= now();
}

/** Is the lease currently held? (Drives whether the reconciler needs to poll.) */
export function isLeaseHeld(): boolean {
  reapExpired();
  return holder !== null;
}

/**
 * Reconcile the lease against what the editor actually reports — the keep-alive.
 * Releases the holder early (well before the TTL) when its PIE session is gone:
 *   - confirmed running, then stopped  → `pie_ended` (crash / manual stop)
 *   - never came up within startup grace → `pie_failed_start`
 *   - editor unreachable past down grace → `editor_down`
 * A holder that is genuinely still running keeps the lease (TTL remains the
 * backstop for a truly-stuck-but-live session).
 */
export function noteLiveness(state: LivenessState): {
  released: boolean;
  reason?: EvictReason;
} {
  reapExpired();
  if (!holder) return { released: false };
  const t = now();

  if (state === "unreachable") {
    holder.unreachableSince ??= t;
    if (t - holder.unreachableSince > config.pieEditorDownGraceMs) {
      return evictHolder("editor_down");
    }
    return { released: false };
  }

  holder.unreachableSince = undefined; // reachable again — reset the down clock

  // Until the handler has actually started PIE for this holder, ignore PIE state
  // (a stale session is being torn down / our session is still booting).
  if (holder.startedAt == null) return { released: false };

  if (state === "running") {
    holder.confirmedRunningAt ??= t;
    return { released: false };
  }

  // state === "stopped"
  if (holder.confirmedRunningAt != null) return evictHolder("pie_ended");
  if (t - holder.startedAt > config.pieStartupGraceMs) {
    return evictHolder("pie_failed_start");
  }
  return { released: false }; // still within the startup grace
}

/** Was this session reaped/evicted while it thought it held the lease? */
export function wasEvicted(sessionId: string): boolean {
  reapExpired();
  return evicted.has(sessionId);
}

/**
 * A session's transport closed. Release its lease and/or drop its queue slot so a
 * disconnected agent never wedges the lease, and a reconnect re-queues at the back.
 */
export function onSessionClosed(sessionId: string): void {
  let changed = false;
  if (holder?.sessionId === sessionId) {
    log.info(`PIE holder session ${short(sessionId)} disconnected; releasing lease`);
    holder = null;
    changed = true;
  }
  if (removeFromQueue(sessionId)) changed = true;
  evicted.delete(sessionId);
  if (changed) notify();
}

/** Read-only view for diagnostics (folded into pie_get_state). */
export function snapshot(sessionId?: string): LeaseSnapshot {
  reapExpired();
  return {
    holder: publicHolder(),
    queue: queue.map((w, i) => ({
      session_id: w.sessionId,
      label: w.label,
      enqueued_at: w.enqueuedAt,
      position: i + 1,
    })),
    queue_length: queue.length,
    lease_ttl_ms: config.pieLeaseTtlMs,
    your_position: sessionId ? positionOf(sessionId) : null,
    you_hold: !!sessionId && holder?.sessionId === sessionId,
  };
}

// ── test hooks (not used in production) ─────────────────────────────────────
export const __test = {
  setClock(fn: () => number) {
    clock = fn;
  },
  resetClock() {
    clock = () => Date.now();
  },
  reset() {
    holder = null;
    queue.length = 0;
    evicted.clear();
    wakeups.clear();
  },
};
