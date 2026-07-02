/**
 * Domain: build — observe cross-agent C++ build coordination.
 *
 * The build lock itself is driven by the build SCRIPTS over the /build REST
 * endpoints (see src/build/http.ts) — not by MCP tools. What agents need from the
 * MCP is the READ side: "is someone building right now, and is the editor up?".
 * That single answer resolves the most common confusion — a build closes the
 * editor, so bridge calls fail; that is expected, not a dead session.
 */

import { z } from "zod";
import type { ToolContext, ToolDef } from "../registry/types.ts";
import { defineTool } from "./_shared.ts";
import { isRecord } from "../bridge/envelope.ts";
import { status as buildLockStatus } from "../build/lock.ts";

const buildStatus = defineTool({
  name: "build_status",
  domain: "build",
  description:
    "Check whether a C++ build is in progress (held by another agent's build " +
    "script) AND whether the editor is reachable — one call to understand the " +
    "world before assuming a session died. A build recompiles with the editor " +
    "CLOSED, so bridge/editor calls failing during a build is EXPECTED, not a " +
    "crash. Returns build {in_progress, holder:{label,pid,target,started,pid_alive}} " +
    "and editor {reachable, ready}. If editor.reachable is false and " +
    "build.in_progress is true, wait for the build to finish and retry.",
  input: z.object({
    random_string: z.string().default("").describe("Unused placeholder (no-arg tool)."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  handler: async (_args, ctx: ToolContext) => {
    const build = buildLockStatus();
    // Quick editor liveness probe (mcp_status bypasses the boot gate).
    let editor = { reachable: false, ready: false };
    const env = await ctx.conn.sendCommand("mcp_status", {});
    if (env.status === "success") {
      editor = {
        reachable: true,
        ready: isRecord(env.result) && env.result.ready === true,
      };
    }
    return { status: "success", result: { build, editor } };
  },
});

export const buildTools: ToolDef[] = [buildStatus];
