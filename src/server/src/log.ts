/**
 * Minimal leveled logger → stderr (stdout stays clean for any tooling that
 * scrapes it). Honors UNREAL_MCP_LOG_LEVEL via {@link config}.
 */

import { config } from "./config.ts";

const ORDER = { debug: 10, info: 20, warn: 30, error: 40 } as const;
const threshold = ORDER[config.logLevel];

function emit(level: keyof typeof ORDER, msg: string): void {
  if (ORDER[level] < threshold) return;
  process.stderr.write(`[${level}] ${msg}\n`);
}

export const log = {
  debug: (m: string) => emit("debug", m),
  info: (m: string) => emit("info", m),
  warn: (m: string) => emit("warn", m),
  error: (m: string) => emit("error", m),
};
