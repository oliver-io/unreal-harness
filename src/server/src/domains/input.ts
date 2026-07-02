/**
 * Domain: input — Enhanced Input asset factory.
 *
 * Port of `input_create` in `src/MCP/server.py`. Wire command equals the tool
 * name. Asset mutator: refused during PIE and does not support dry_run.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const inputCreate = bridgeTool({
  name: "input_create",
  domain: "input",
  description:
    "Create a new Enhanced Input asset — UInputAction ('action') or UInputMappingContext " +
    "('mapping_context'/'imc'). Auto-saves; path uniqueness enforced. value_type is action-only. " +
    "Specify destination via (path + name) or asset_path.",
  input: z.object({
    type: z
      .string()
      .min(1)
      .describe('Closed set: "action" → UInputAction, "mapping_context" (alias "imc") → UInputMappingContext. Required.'),
    path: z.string().default("").describe('Destination package path. Pair with name.'),
    name: z.string().default("").describe("Asset short name."),
    asset_path: z.string().default("").describe("Convenience combined path; replaces (path, name)."),
    value_type: z
      .string()
      .default("")
      .describe(
        'Action-only. Closed set: "boolean" (alias "bool"/"digital", default), "axis1d" (alias "float"), ' +
          '"axis2d" (alias "vector2d"/"vec2"), "axis3d" (alias "vector"/"vec3").',
      ),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  // type always sent; the rest omitted when empty (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = { type: a.type };
    if (a.path) p.path = a.path;
    if (a.name) p.name = a.name;
    if (a.asset_path) p.asset_path = a.asset_path;
    if (a.value_type) p.value_type = a.value_type;
    return p;
  },
});

const inputAddMapping = bridgeTool({
  name: "input_add_mapping",
  domain: "input",
  description:
    "Add one or more key→action mapping rows to an existing UInputMappingContext (the wiring " +
    "input_create can't do). Calls UInputMappingContext::MapKey(action, key) per key, appending " +
    "FEnhancedActionKeyMapping rows to the IMC's default mappings, then auto-saves. Keys are EKeys " +
    "names (e.g. 'W', 'SpaceBar', 'LeftMouseButton', 'Gamepad_FaceButton_Bottom') — an unknown key " +
    "name is rejected with invalid_argument before any mutation. Provide a single `key` and/or a " +
    "`keys` array (validated all-or-nothing). Returns keys_added + the IMC's total mappings_count.",
  input: z.object({
    context_path: z
      .string()
      .min(1)
      .describe("Asset path of the UInputMappingContext to mutate, e.g. '/Game/Input/IMC_Default'."),
    action_path: z
      .string()
      .min(1)
      .describe("Asset path of the UInputAction to bind, e.g. '/Game/Input/IA_Jump'."),
    key: z
      .string()
      .default("")
      .describe("A single EKeys name to map (e.g. 'SpaceBar'). Combine with or replace by `keys`."),
    keys: z
      .array(z.string())
      .default([])
      .describe("Batch of EKeys names to map to the same action. Merged with `key` if both given."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      context_path: a.context_path,
      action_path: a.action_path,
    };
    if (a.key) p.key = a.key;
    if (a.keys && a.keys.length) p.keys = a.keys;
    return p;
  },
});

export const inputTools: ToolDef[] = [inputCreate, inputAddMapping];
