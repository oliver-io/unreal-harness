/**
 * Closed-set error codes — TypeScript mirror of the C++ `EMCPErrorCode` enum
 * (`Plugins/UnrealMCP/Source/UnrealMCP/Public/Commands/MCPCommonUtils.h`).
 *
 * Adding a code requires a synchronized change here AND in the C++ enum +
 * `MCPErrorCodeToString` switch. Wire form is flat snake_case, carried on the
 * envelope as `error_code` (see {@link ./envelope.ts}).
 *
 * Port of `src/MCP/helpers/error_codes.py` — kept in exact parity with it.
 */

export const ErrorCode = {
  // ── Identity / lookup ──────────────────────────────────────────────
  ASSET_NOT_FOUND: "asset_not_found",
  CLASS_NOT_LOADED: "class_not_loaded",
  NODE_NOT_FOUND: "node_not_found",
  ACTOR_NOT_FOUND: "actor_not_found",
  VARIABLE_NOT_FOUND: "variable_not_found",
  PIN_NOT_FOUND: "pin_not_found",
  FUNCTION_NOT_FOUND: "function_not_found",
  UNKNOWN_TAG: "unknown_tag",
  WINDOW_NOT_FOUND: "window_not_found",
  AMBIGUOUS_TARGET: "ambiguous_target",

  // ── Input shape ────────────────────────────────────────────────────
  INVALID_ARGUMENT: "invalid_argument",
  INVALID_PATH: "invalid_path",
  INVALID_PIN_TYPE: "invalid_pin_type",
  OUT_OF_RANGE: "out_of_range",
  UNSUPPORTED_CLASS: "unsupported_class",

  // ── State / preconditions ──────────────────────────────────────────
  NOT_IN_PIE: "not_in_pie",
  PIE_ACTIVE: "pie_active",
  EDITOR_NOT_READY: "editor_not_ready",
  DRY_RUN_UNSUPPORTED: "dry_run_unsupported",
  FEATURE_DISABLED: "feature_disabled",
  ENGINE_BUSY: "engine_busy",
  NAME_COLLISION: "name_collision",
  ASSET_DIRTY: "asset_dirty",
  ASSET_LOCKED: "asset_locked",
  COMPILE_IN_PROGRESS: "compile_in_progress",
  LIVE_CODING_UNAVAILABLE: "live_coding_unavailable",

  // ── Relationships / integrity ──────────────────────────────────────
  CIRCULAR_DEPENDENCY: "circular_dependency",
  WOULD_BREAK_REFERENCES: "would_break_references",

  // ── Execution / transport ──────────────────────────────────────────
  ASSET_COMPILE_FAILED: "asset_compile_failed",
  TIMEOUT: "timeout",
  INTERNAL: "internal",
} as const;

export type ErrorCode = (typeof ErrorCode)[keyof typeof ErrorCode];

const ALL_CODES = new Set<string>(Object.values(ErrorCode));

/** Narrow an arbitrary wire string to a known {@link ErrorCode}, else undefined. */
export function asErrorCode(value: unknown): ErrorCode | undefined {
  return typeof value === "string" && ALL_CODES.has(value)
    ? (value as ErrorCode)
    : undefined;
}
