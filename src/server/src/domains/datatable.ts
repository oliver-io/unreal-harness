/**
 * Domain: datatable — DataTable asset factory.
 *
 * Port of `datatable_create` in `src/MCP/server.py`. Wire command equals the
 * tool name. Asset mutator: refused during PIE and does not support dry_run.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const datatableCreate = bridgeTool({
  name: "datatable_create",
  domain: "datatable",
  description:
    "Create a new UDataTable asset bound to a row UScriptStruct. Auto-saves; path " +
    "uniqueness enforced. row_struct (required) must resolve before the factory runs. " +
    "Specify destination via (path + name) or asset_path.",
  input: z.object({
    row_struct: z
      .string()
      .min(1)
      .describe(
        'Path or name of the row UScriptStruct. Accepts asset paths ("/Game/Data/FFoo"), ' +
          'script paths ("/Script/Engine.SlateBrush"), or short names ("FCraftingRecipe", F optional). Required.',
      ),
    path: z.string().default("").describe('Destination package path ("/Game/Data/Tables"). Pair with name.'),
    name: z.string().default("").describe('Asset short name ("DT_Recipes").'),
    asset_path: z.string().default("").describe("Convenience combined path; replaces (path, name)."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  // row_struct always sent; path/name/asset_path omitted when empty (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = { row_struct: a.row_struct };
    if (a.path) p.path = a.path;
    if (a.name) p.name = a.name;
    if (a.asset_path) p.asset_path = a.asset_path;
    return p;
  },
});

export const datatableTools: ToolDef[] = [datatableCreate];
