/**
 * Domain: project — project-level metadata.
 *
 * Port of `project_context` in `src/MCP/server.py`. Wire command equals the
 * tool name; no args. Read-only.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const projectContext = bridgeTool({
  name: "project_context",
  domain: "project",
  description:
    "Compact project-level metadata: name, engine_version, default_map, plugins[], modules[], " +
    "and settings_paths. Read-only orient-rung tool — pulls from already-loaded descriptors " +
    "(no asset loads, no level scans).",
  input: z.object({}),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

export const projectTools: ToolDef[] = [projectContext];
