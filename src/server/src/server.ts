/**
 * Build the MCP server from the tool registry.
 *
 * Each {@link ToolDef} becomes an SDK tool: the Zod input schema drives both the
 * advertised JSON Schema and validation (the SDK validates before our handler
 * runs), and the handler's JSON payload is surfaced as a text content block plus
 * `structuredContent` for clients that consume it.
 *
 * Tier-2 meta-tools and the Tier-3 `run` tool register here too (added in P3) —
 * they are just more registry-backed tools.
 */

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import type { ToolRegistry } from "./registry/index.ts";
import type { ToolContext, ToolDef } from "./registry/types.ts";
import { getConnection } from "./bridge/connection.ts";
import { applyAliases } from "./registry/aliases.ts";
import { envelopeError } from "./bridge/envelope.ts";
import { maybeCompact } from "./compaction/handles.ts";
import { config } from "./config.ts";

export const SERVER_INFO = { name: "unreal-harness", version: "0.1.0" } as const;

// Always-on plumbing domains (not Unreal ops): readiness, discovery, slicing.
const ALWAYS = new Set(["core", "catalog", "result"]);

/** Which tools a surface mode advertises (progressive disclosure — see config). */
function exposedBy(mode: typeof config.surface): (d: ToolDef) => boolean {
  if (mode === "compact") return (d) => ALWAYS.has(d.domain);
  if (mode === "code") return (d) => ALWAYS.has(d.domain) || d.domain === "code";
  return (d) => d.domain !== "code"; // full: everything except opt-in code-mode
}

export function buildServer(
  registry: ToolRegistry,
  opts?: { surface?: typeof config.surface },
): McpServer {
  const server = new McpServer(SERVER_INFO);
  const conn = getConnection();
  const expose = exposedBy(opts?.surface ?? config.surface);

  for (const def of registry.all().filter(expose)) {
    server.registerTool(
      def.name,
      {
        description: def.description,
        // SDK builds the JSON Schema + validates from this ZodRawShape. Empty
        // shape == no-args tool.
        inputSchema: def.input.shape,
        annotations: toMcpAnnotations(def.name, def.annotations),
      },
      async (args: unknown, extra) => {
        // Per-call context: the SDK gives us this request's session id (one per
        // agent) and an abort signal that also fires on transport close. Both
        // feed cross-session coordination (the PIE lease).
        const ctx: ToolContext = {
          conn,
          sessionId: extra?.sessionId,
          signal: extra?.signal,
        };
        let payload: unknown;
        try {
          // Fold any concept-aliased params (e.g. blueprint_name → blueprint_path)
          // onto their canonical key before the handler sees them (GAP-021/041).
          const normalized = applyAliases(
            (args ?? {}) as Record<string, unknown>,
            def.aliases,
          );
          payload = await def.handler(normalized as never, ctx);
        } catch (err) {
          payload = envelopeError(err);
        }
        // Result compaction is a no-op unless config.maxResultBytes > 0; never
        // applied to plumbing tools (their results are already small/meta).
        if (!ALWAYS.has(def.domain) && def.domain !== "code") {
          payload = maybeCompact(payload, config.maxResultBytes);
        }
        // Parity with the Python/FastMCP server: a structured bridge error
        // (status:"error" + error_code) is a SUCCESSFUL tool execution that
        // returned an error envelope — the envelope's `status` is the error
        // channel, not the MCP protocol-level `isError` flag. (Reserve isError
        // for the SDK's own validation failures, which it raises before us.)
        return {
          content: [{ type: "text" as const, text: JSON.stringify(payload) }],
        };
      },
    );
  }

  return server;
}

/** Project our annotations onto the standard MCP set (custom hints stay internal). */
function toMcpAnnotations(
  title: string,
  a: import("./registry/types.ts").ToolAnnotations | undefined,
) {
  return {
    title,
    readOnlyHint: a?.readOnlyHint,
    destructiveHint: a?.destructiveHint,
    idempotentHint: a?.idempotentHint,
  };
}
