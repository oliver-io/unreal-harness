/**
 * Domain: scene — editor-world summary.
 *
 * Port of `scene_brief` in `src/MCP/server.py`. Wire command equals the tool
 * name; no args. Read-only.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const sceneBrief = bridgeTool({
  name: "scene_brief",
  domain: "scene",
  description:
    "Compact summary of the current editor world: total_actors, by_class counts (descending), " +
    "distinct_classes, loaded_levels_count, and skipped_sublevels (streamed sublevels that exist " +
    "but are not loaded — not force-loaded by this tool). Read-only.",
  input: z.object({}),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

export const sceneTools: ToolDef[] = [sceneBrief];
