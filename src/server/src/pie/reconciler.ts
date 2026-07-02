/**
 * PIE lease keep-alive.
 *
 * The lease ({@link ./lease.ts}) is keyed by MCP client session — a different
 * process from both the editor and the PIE world — so a PIE crash or an editor
 * crash is invisible to it: the lease would stay "held" until the 10-min TTL,
 * stalling every other agent in the meantime. This reconciler closes that gap.
 *
 * While the lease is held it polls the editor's actual PIE state and feeds it to
 * `noteLiveness`, which releases the holder early when its session is gone
 * (crashed, manually stopped, never started, or the editor went away). When no
 * lease is held it does no bridge work — zero steady-state cost.
 *
 * The probe is gate-aware: `mcp_status` (which bypasses the boot gate) first, so a
 * down/booting editor is reported as `unreachable` immediately instead of blocking
 * the tick on the readiness gate.
 */

import { getConnection } from "../bridge/connection.ts";
import { isRecord } from "../bridge/envelope.ts";
import { config } from "../config.ts";
import { log } from "../log.ts";
import { isLeaseHeld, noteLiveness, type LivenessState } from "./lease.ts";

async function probe(): Promise<LivenessState> {
  // mcp_status bypasses the boot gate → fast, and never blocks on readiness.
  const status = await getConnection().sendCommand("mcp_status", {});
  if (status.status === "error") return "unreachable";
  const ready = isRecord(status.result) && status.result.ready === true;
  if (!ready) return "unreachable"; // editor down or still booting → not usable

  const env = await getConnection().sendCommand("pie_get_state", {});
  if (env.status === "error") return "unreachable";
  return isRecord(env.result) && env.result.is_running === true ? "running" : "stopped";
}

/** Start the keep-alive loop. Returns a stopper for clean shutdown. */
export function startPieReconciler(): () => void {
  let stopped = false;
  let timer: ReturnType<typeof setTimeout> | undefined;

  const tick = async (): Promise<void> => {
    if (stopped) return;
    try {
      if (isLeaseHeld()) {
        const verdict = noteLiveness(await probe());
        if (verdict.released) {
          log.info(`PIE reconciler reclaimed the lease (${verdict.reason})`);
        }
      }
    } catch (err) {
      log.debug(
        `PIE reconciler probe failed: ${err instanceof Error ? err.message : String(err)}`,
      );
    }
    if (!stopped) timer = setTimeout(tick, config.pieLivenessPollMs);
  };

  timer = setTimeout(tick, config.pieLivenessPollMs);
  log.info(`PIE lease reconciler started (poll ${config.pieLivenessPollMs}ms while held)`);
  return () => {
    stopped = true;
    if (timer) clearTimeout(timer);
  };
}
