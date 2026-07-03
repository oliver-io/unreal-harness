/**
 * Helpers shared by domain modules.
 *
 * Most tools are a thin, declarative forward: validate args → send them to the
 * bridge under the canonical command name (which, post naming-migration, equals
 * the tool name) → return the envelope. {@link bridgeTool} captures that shape
 * so a domain file reads as a list of operations, not boilerplate.
 *
 * Tools that need to transform args, fan out to multiple commands, or post-
 * process the result define a `handler` directly instead.
 */

import type { z } from "zod";
import type { ToolDef, ToolAnnotations } from "../registry/types.ts";

export interface BridgeToolSpec<S extends z.ZodObject<z.ZodRawShape>> {
  name: string;
  domain: string;
  description: string;
  input: S;
  /** Wire command; defaults to `name` (canonical == wire post-migration). */
  command?: string;
  annotations?: ToolAnnotations;
  /** Map validated args → bridge params. Defaults to identity. */
  params?: (args: z.infer<S>) => Record<string, unknown>;
}

/**
 * Build a forwarding tool: validated args → bridge command → envelope.
 *
 * Returns the generic-erased {@link ToolDef} so tools of differing shapes share
 * one `ToolDef[]` (the handler's arg type is contravariant, so a specific shape
 * isn't assignable to the erased array element otherwise). DX inside `params`
 * still gets the precise arg type via `S`.
 */
export function bridgeTool<S extends z.ZodObject<z.ZodRawShape>>(
  spec: BridgeToolSpec<S>,
): ToolDef {
  const command = spec.command ?? spec.name;
  const def: ToolDef<S> = {
    name: spec.name,
    domain: spec.domain,
    description: spec.description,
    input: spec.input,
    annotations: spec.annotations,
    command,
    handler: (args, ctx) =>
      ctx.conn.sendCommand(
        command,
        spec.params ? spec.params(args) : (args as Record<string, unknown>),
      ),
  };
  return def as unknown as ToolDef;
}

/**
 * Erase the generic on a fully-typed, custom-handler tool so it shares a
 * `ToolDef[]` with others. Use for tools that transform results or fan out to
 * multiple bridge commands (anything {@link bridgeTool} can't express).
 */
export function defineTool<S extends z.ZodObject<z.ZodRawShape>>(
  def: ToolDef<S>,
): ToolDef {
  return def as unknown as ToolDef;
}
