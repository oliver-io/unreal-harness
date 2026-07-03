/**
 * Tool surface as data. Every Unreal operation is a {@link ToolDef} in the
 * registry — a Zod input schema (the single source of truth for JSON Schema +
 * runtime validation + TS types), annotations, and a handler. Disclosure tiers
 * (client tool-search, server meta-tools, code-execution mode) and result
 * compaction are all layers over this same registry.
 */

import type { z } from "zod";
import type { UnrealConnection } from "../bridge/connection.ts";

/** Standard MCP tool annotations (hints for clients; also drive our gates). */
export interface ToolAnnotations {
  /** Does not mutate editor/asset state. */
  readOnlyHint?: boolean;
  /** May irreversibly change state (delete/overwrite). */
  destructiveHint?: boolean;
  /** Repeated identical calls have no additional effect. */
  idempotentHint?: boolean;
  /** Refused while a PIE session is active. */
  blockedDuringPie?: boolean;
  /** Refuses `dry_run:true` with `dry_run_unsupported`. */
  dryRunUnsupported?: boolean;
}

export interface ToolContext {
  conn: UnrealConnection;
  /**
   * MCP session id of the calling client (one per agent), when the transport
   * provides one. Used by cross-session coordination (the PIE lease). Undefined
   * for sessionless clients → coordination degrades to single-agent behaviour.
   */
  sessionId?: string;
  /**
   * Per-call abort signal. Fires when the client cancels the request OR its
   * transport closes — the PIE lease treats that as "agent gone, free its slot".
   */
  signal?: AbortSignal;
}

/**
 * One tool. `input` is a Zod object; `handler` receives the parsed/validated
 * args and returns the JSON payload to surface to the client (typically a
 * bridge envelope or its inner result). Throwing is allowed — the server layer
 * converts a throw into an error envelope.
 */
export interface ToolDef<
  S extends z.ZodObject<z.ZodRawShape> = z.ZodObject<z.ZodRawShape>,
> {
  name: string;
  domain: string;
  /** One-line, high-signal: this IS the LLM's guidance. Keep it accurate. */
  description: string;
  /** Always a `z.object(...)` so `.shape` feeds the SDK's tool schema. */
  input: S;
  annotations?: ToolAnnotations;
  /**
   * Concept-level parameter aliases (alias key → canonical key). When set, the
   * tool's `input` already accepts the alias keys; every dispatch site runs
   * {@link applyAliases} on the validated args to fold them onto the canonical
   * key before `handler`. See `registry/aliases.ts` (GAP-021/041).
   */
  aliases?: Record<string, string>;
  /**
   * Bridge wire command this tool forwards to. Set by `bridgeTool` (equals
   * `name` post naming-migration, except the legacy `command:` overrides such
   * as `statetree_node_add` → `st_add_node`). Unset on custom-handler tools
   * (`defineTool`): server-local tools and composites. The bun coverage gate
   * (`test/coverage.test.ts`) keys off `command ?? name` vs the C++ dispatch
   * keys to split bridge-tier (pytest `@covers`) from server-local (bun)
   * coverage.
   */
  command?: string;
  handler: (args: z.infer<S>, ctx: ToolContext) => Promise<unknown> | unknown;
}

/** Lightweight projection used by search/list (no schema payload). */
export interface ToolSummary {
  name: string;
  domain: string;
  description: string;
  annotations?: ToolAnnotations;
}
