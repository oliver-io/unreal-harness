/**
 * The uniform bridge response envelope and helpers to read it.
 *
 * The Unreal C++ bridge (`MCPBridge.cpp::ExecuteCommand`) wraps every handler
 * response as:
 *
 *     { "status": "success" | "error", "result": {...}, "error": "...",
 *       "error_code": "asset_not_found", "error_hint": "..." }
 *
 * Callers branch on the OUTER envelope's `status` — never the inner result's
 * `success` key, which individual handlers set inconsistently.
 *
 * Port of `src/MCP/helpers/common.py`.
 */

import { asErrorCode, type ErrorCode } from "./errors.ts";

export interface Envelope {
  status: "success" | "error";
  result?: unknown;
  error?: string;
  error_code?: string;
  error_hint?: string;
}

/**
 * Build a bridge-envelope-compatible error for Python/TS-side failures — a
 * local exception, socket error, or JSON decode error that has no handler
 * response to forward. Keeps the envelope seen by MCP clients uniform with the
 * bridge's native error path.
 */
export function envelopeError(
  message: unknown,
  opts?: { code?: ErrorCode; hint?: string },
): Envelope {
  const env: Envelope = {
    status: "error",
    error: message instanceof Error ? message.message : String(message),
  };
  if (opts?.code) env.error_code = opts.code;
  if (opts?.hint) env.error_hint = opts.hint;
  return env;
}

/** True iff the bridge envelope reports success. Tolerates null/non-object. */
export function bridgeOk(response: unknown): response is Envelope {
  return isRecord(response) && response["status"] === "success";
}

/**
 * The envelope's inner `result` object. Handler-emitted fields (`node_id`,
 * `actors`, …) live here, not on the outer envelope. Returns an empty object
 * when `result` is missing/null/non-object so call sites can index without a
 * guard.
 */
export function bridgeInner(response: unknown): Record<string, unknown> {
  if (!isRecord(response)) return {};
  const inner = response["result"];
  return isRecord(inner) ? inner : {};
}

/** The structured error code on a failed envelope, narrowed to the closed set. */
export function bridgeErrorCode(response: unknown): ErrorCode | undefined {
  return isRecord(response) ? asErrorCode(response["error_code"]) : undefined;
}

/** The optional human-readable recovery hint paired with `error_code`. */
export function bridgeErrorHint(response: unknown): string {
  if (!isRecord(response)) return "";
  const hint = response["error_hint"];
  return typeof hint === "string" ? hint : "";
}

export function isRecord(v: unknown): v is Record<string, unknown> {
  return typeof v === "object" && v !== null && !Array.isArray(v);
}
