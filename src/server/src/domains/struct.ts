/**
 * Domain: struct — UserDefinedStruct asset factory.
 *
 * Port of `struct_create` in `src/MCP/server.py`. Wire command equals the tool
 * name. Asset mutator: refused during PIE and does not support dry_run.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const structCreate = bridgeTool({
  name: "struct_create",
  domain: "struct",
  description:
    "Create a new UserDefinedStruct (UUserDefinedStruct) asset shell. Auto-saves. Field editing " +
    "post-create is deferred — UE seeds one default-typed member so the struct is non-empty. " +
    "Specify destination via (path + name) or asset_path.",
  input: z.object({
    path: z.string().default("").describe('Destination package path ("/Game/Data/Structs"). Pair with name.'),
    name: z.string().default("").describe('Asset short name ("FFoo").'),
    asset_path: z.string().default("").describe("Convenience combined path."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  // All keys omitted when empty (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.path) p.path = a.path;
    if (a.name) p.name = a.name;
    if (a.asset_path) p.asset_path = a.asset_path;
    return p;
  },
});

export const structTools: ToolDef[] = [structCreate];
