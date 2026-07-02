/**
 * Process lifecycle guards — Bun port of `src/MCP/helpers/lifecycle.py`.
 *
 * Under streamable-HTTP the server is a long-lived standalone process; the
 * client and the editor bridge come and go independently and neither should
 * tear us down. We install:
 *   1. an engine-health watchdog (LOG-ONLY — a missing editor is transient),
 *   2. signal handlers (SIGINT/SIGTERM/SIGBREAK) for clean shutdown,
 *   3. a final cleanup hook.
 */

import { config } from "../config.ts";
import { log } from "../log.ts";

const PROBE_INTERVAL_MS = 5_000;
const PROBE_TIMEOUT_MS = 2_000;
const PROBE_GRACE_MS = 15_000;

let shuttingDown = false;
let cleanup: (() => void) | undefined;

/** Probe the bridge: "down" | "initializing" | "interactive". */
async function probeEngineState(): Promise<"down" | "initializing" | "interactive"> {
  let socket: Awaited<ReturnType<typeof Bun.connect>> | undefined;
  return new Promise((resolve) => {
    let settled = false;
    const chunks: Uint8Array[] = [];
    const finish = (s: "down" | "initializing" | "interactive") => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      try {
        socket?.end();
      } catch {
        /* closed */
      }
      resolve(s);
    };
    const timer = setTimeout(() => finish(chunks.length ? "initializing" : "down"), PROBE_TIMEOUT_MS);

    Bun.connect({
      hostname: config.bridgeHost,
      port: config.bridgePort,
      socket: {
        open(s) {
          s.write(JSON.stringify({ type: "mcp_status", params: {} }));
        },
        data(_s, data) {
          chunks.push(data);
          try {
            const resp = JSON.parse(new TextDecoder().decode(concat(chunks)));
            const ready = resp?.result?.ready === true;
            finish(ready ? "interactive" : "initializing");
          } catch {
            /* partial — keep waiting for timeout or more data */
          }
        },
        error: () => finish("down"),
        connectError: () => finish("down"),
      },
    })
      .then((s) => {
        socket = s;
      })
      .catch(() => finish("down"));
  });
}

async function watchdogLoop(): Promise<void> {
  await sleep(PROBE_GRACE_MS);
  let last: string | undefined;
  while (!shuttingDown) {
    const state = await probeEngineState();
    if (state !== last) {
      if (state === "down") log.warn("engine bridge unreachable — tool calls will fail until the editor is back");
      else if (state === "initializing") log.info("editor initializing — real tool calls PEND at the boot gate");
      else log.info("editor interactive — boot gate open");
      last = state;
    }
    await sleep(PROBE_INTERVAL_MS);
  }
}

function shutdown(reason: string): void {
  if (shuttingDown) return;
  shuttingDown = true;
  log.warn(`shutting down — ${reason}`);
  try {
    cleanup?.();
  } catch {
    /* ignore */
  }
  process.exit(0);
}

export function startLifecycleGuards(onCleanup?: () => void): void {
  cleanup = onCleanup;
  process.on("SIGINT", () => shutdown("SIGINT"));
  process.on("SIGTERM", () => shutdown("SIGTERM"));
  process.on("SIGBREAK", () => shutdown("SIGBREAK"));
  void watchdogLoop();
  log.info(
    `lifecycle guards armed (probe tcp://${config.bridgeHost}:${config.bridgePort}, ` +
      `interval ${PROBE_INTERVAL_MS / 1000}s, mode=log-only)`,
  );
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

function concat(chunks: Uint8Array[]): Uint8Array {
  if (chunks.length === 1) return chunks[0]!;
  const total = chunks.reduce((n, c) => n + c.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const c of chunks) {
    out.set(c, off);
    off += c.length;
  }
  return out;
}
