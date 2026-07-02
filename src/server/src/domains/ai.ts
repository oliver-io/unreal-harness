/**
 * Domain: ai — runtime inspection of AI actors (require PIE).
 *
 * Port of the `ai_*` runtime-inspection tools in `src/MCP/server.py`. Each is a
 * thin 1:1 forward through the `ai_eqs_runtime` helper to a wire command of the
 * same name, passing `{ actor_name }` unchanged.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const aiGetState = bridgeTool({
  name: "ai_get_state",
  domain: "ai",
  description:
    "Inspect the runtime state of an AI actor — StateTree status, active states. Requires PIE.",
  input: z.object({
    actor_name: z.string().min(1).describe("Name of the AI actor to inspect."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const aiGetAwareness = bridgeTool({
  name: "ai_get_awareness",
  domain: "ai",
  description:
    "Inspect an AI's CombatAwarenessComponent — target memory, visibility. Requires PIE.",
  input: z.object({
    actor_name: z.string().min(1).describe("Name of the AI actor to inspect."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const aiGetPerception = bridgeTool({
  name: "ai_get_perception",
  domain: "ai",
  description:
    "Inspect an AI's perception component — perceived actors, stimuli, sense configs. Requires PIE.",
  input: z.object({
    actor_name: z.string().min(1).describe("Name of the AI actor to inspect."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

export const aiTools: ToolDef[] = [aiGetState, aiGetAwareness, aiGetPerception];
