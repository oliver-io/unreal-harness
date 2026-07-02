/**
 * Domain: ik_rig — IK Rig definition reads.
 *
 * Port of `ik_rig_list_chains` in `src/MCP/server.py`. Wire command equals the
 * tool name; 1:1 params. Read-only.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const ikRigListChains = bridgeTool({
  name: "ik_rig_list_chains",
  domain: "ik_rig",
  description:
    "Read retarget chain definitions from a UIKRigDefinition asset: per chain name, " +
    "start/end bones, attached IK goal, plus the rig's pelvis bone and preview mesh. " +
    "Use to discover chain names before wiring ik_retarget_set_chain_mapping.",
  input: z.object({
    ik_rig_path: z.string().min(1).describe("Asset path of a UIKRigDefinition."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

export const ik_rigTools: ToolDef[] = [ikRigListChains];
