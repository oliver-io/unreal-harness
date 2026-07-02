/**
 * Build lock — single-machine serialization of C++ builds across agents.
 *
 * Building the editor target recompiles the plugin + project and, crucially, runs
 * with the **editor closed** (it locks the editor DLLs). So while one agent builds,
 * every bridge call from another agent fails — and agents misread that as "my
 * session died". This lock makes builds take turns and makes the in-progress build
 * visible, so the editor's absence is understood as "a build is running", not a crash.
 *
 * Substrate differs from the PIE lease: builds are driven by external build SCRIPTS
 * (separate OS processes), not in-band MCP calls. So the lock is acquired/released
 * over plain HTTP (see ./http.ts) by the scripts, which send their own **PID**.
 * Because everything here is localhost, the server can check that PID directly —
 * a crashed/aborted build frees the lock the instant anyone touches the lock again
 * (lazy reap). The TTL is only a backstop for a build that hangs while alive.
 *
 * Exclusive by design: a second concurrent build is REFUSED (with the holder's
 * details) rather than queued — builds are long and a shell script blocking on a
 * queue is fragile; the caller retries when `build_status` shows the lock free.
 *
 * In-memory only: a server restart clears the lock (everyone re-coordinates).
 */

import { randomUUID } from "node:crypto";
import { hostname } from "node:os";
import { config } from "../config.ts";
import { log } from "../log.ts";

/** Injectable for tests (real wall clock / real PID check in production). */
let clock: () => number = () => Date.now();
let pidAlive: (pid: number) => boolean = defaultPidAlive;
const now = () => clock();

const SELF_HOST = hostname();

function defaultPidAlive(pid: number): boolean {
  if (!Number.isInteger(pid) || pid <= 0) return false;
  try {
    // Signal 0 doesn't kill — it only probes existence/permission.
    process.kill(pid, 0);
    return true;
  } catch (err) {
    // ESRCH → no such process (dead); EPERM → exists but not ours (alive).
    return (err as NodeJS.ErrnoException)?.code === "EPERM";
  }
}

interface Build {
  buildId: string;
  pid: number;
  host: string;
  target: string;
  label: string;
  startedAt: number;
  expiresAt: number;
}

let current: Build | null = null;

export interface BuildView {
  build_id: string;
  pid: number;
  host: string;
  target: string;
  label: string;
  started_at: number;
  expires_at: number;
  held_ms: number;
  expires_in_ms: number;
  /** Whether the build process is still alive (localhost PID check). */
  pid_alive: boolean;
}

export interface AcquireOpts {
  pid: number;
  target?: string;
  label?: string;
  host?: string;
  ttlMs?: number;
}

export interface AcquireResult {
  outcome: "acquired" | "busy";
  /** True when this call newly took the lock (vs. the same builder re-affirming). */
  fresh: boolean;
  build: BuildView;
}

// ── internals ─────────────────────────────────────────────────────────────

/** Only trust a PID check when the builder is on this machine (localhost harness). */
function isLocal(b: Build): boolean {
  return (
    b.host === SELF_HOST ||
    b.host === "" ||
    b.host === "localhost" ||
    b.host === "127.0.0.1"
  );
}

function view(b: Build): BuildView {
  const t = now();
  return {
    build_id: b.buildId,
    pid: b.pid,
    host: b.host,
    target: b.target,
    label: b.label,
    started_at: b.startedAt,
    expires_at: b.expiresAt,
    held_ms: t - b.startedAt,
    expires_in_ms: b.expiresAt - t,
    pid_alive: isLocal(b) ? pidAlive(b.pid) : true,
  };
}

/** Reclaim the lock if the holder's process is gone (crash/abort) or it hung past
 *  the TTL. Runs lazily on every public entry, so a dead build is freed the moment
 *  anyone next interacts — no background poller required. */
function reap(): void {
  if (!current) return;
  if (isLocal(current) && !pidAlive(current.pid)) {
    log.warn(
      `build lock: holder pid ${current.pid} (${current.label}) is gone; reclaiming`,
    );
    current = null;
    return;
  }
  if (now() >= current.expiresAt) {
    log.warn(
      `build lock: holder ${current.label} (pid ${current.pid}) exceeded TTL; reclaiming`,
    );
    current = null;
  }
}

// ── public API ──────────────────────────────────────────────────────────

export function acquire(opts: AcquireOpts): AcquireResult {
  reap();
  const ttl = opts.ttlMs ?? config.buildLockTtlMs;

  // Same builder re-acquiring (idempotent retry / heartbeat) → refresh, not busy.
  if (current && current.pid === opts.pid && isLocal(current)) {
    current.expiresAt = now() + ttl;
    if (opts.target) current.target = opts.target;
    return { outcome: "acquired", fresh: false, build: view(current) };
  }
  if (current) return { outcome: "busy", fresh: false, build: view(current) };

  current = {
    buildId: randomUUID(),
    pid: opts.pid,
    host: opts.host || SELF_HOST,
    target: opts.target || "",
    label: opts.label || `pid ${opts.pid}`,
    startedAt: now(),
    expiresAt: now() + ttl,
  };
  log.info(
    `build lock acquired by ${current.label} (pid ${current.pid}` +
      `${current.target ? `, target ${current.target}` : ""})`,
  );
  return { outcome: "acquired", fresh: true, build: view(current) };
}

export type ReleaseOutcome = "released" | "not_holder" | "already_free";

export function release(opts: { buildId?: string; pid?: number }): {
  outcome: ReleaseOutcome;
} {
  reap();
  if (!current) return { outcome: "already_free" };
  const matches =
    (opts.buildId != null && current.buildId === opts.buildId) ||
    (opts.pid != null && current.pid === opts.pid);
  if (!matches) return { outcome: "not_holder" };
  log.info(`build lock released by ${current.label} (pid ${current.pid})`);
  current = null;
  return { outcome: "released" };
}

export function heartbeat(opts: {
  buildId?: string;
  pid?: number;
  ttlMs?: number;
}): { ok: boolean; build: BuildView | null } {
  reap();
  if (
    current &&
    ((opts.buildId != null && current.buildId === opts.buildId) ||
      (opts.pid != null && current.pid === opts.pid))
  ) {
    current.expiresAt = now() + (opts.ttlMs ?? config.buildLockTtlMs);
    return { ok: true, build: view(current) };
  }
  return { ok: false, build: current ? view(current) : null };
}

export interface BuildStatus {
  in_progress: boolean;
  holder: BuildView | null;
}

export function status(): BuildStatus {
  reap();
  return { in_progress: current !== null, holder: current ? view(current) : null };
}

/** The active build, or null. Used to contextualize editor-unreachable errors. */
export function buildInProgress(): BuildView | null {
  reap();
  return current ? view(current) : null;
}

// ── test hooks ──────────────────────────────────────────────────────────
export const __test = {
  setClock(fn: () => number) {
    clock = fn;
  },
  setPidAlive(fn: (pid: number) => boolean) {
    pidAlive = fn;
  },
  reset() {
    current = null;
    clock = () => Date.now();
    pidAlive = defaultPidAlive;
  },
};
