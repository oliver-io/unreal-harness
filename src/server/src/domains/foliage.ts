/**
 * Domain: foliage — read-only foliage inspection (GAPS #14).
 *
 * Mutation (add type / scatter / remove) is refused by design — procedural,
 * brush-driven content belongs to the editor's Foliage mode. This tool only
 * reads existing foliage. Wire command equals the tool name. Read-only.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const foliageInspect = bridgeTool({
  name: "foliage_inspect",
  domain: "foliage",
  description:
    "Read-only inspection of foliage in the loaded editor world. mode='types' (default) lists " +
    "each foliage type with its placed-instance count and key properties (mesh, density, radius, " +
    "align_to_normal, random_yaw, cull_distance). mode='instances' lists placed instance " +
    "transforms for one foliage_type, bounded by limit/offset (never dumps every instance) and " +
    "returns total_instances + truncated for paging. Mutation is refused.",
  input: z.object({
    mode: z
      .enum(["types", "instances"])
      .default("types")
      .describe("'types' (default) = per-type summary; 'instances' = transforms for one type."),
    foliage_type: z
      .string()
      .default("")
      .describe(
        "Required for mode='instances': a type identity / mesh path / display name from mode='types'.",
      ),
    limit: z
      .number()
      .int()
      .min(1)
      .max(1000)
      .default(100)
      .describe("mode='instances' only: max instances to return (1..1000)."),
    offset: z
      .number()
      .int()
      .min(0)
      .default(0)
      .describe("mode='instances' only: starting instance index for paging."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = { mode: a.mode };
    if (a.mode === "instances") {
      if (a.foliage_type) p.foliage_type = a.foliage_type;
      p.limit = a.limit;
      p.offset = a.offset;
    }
    return p;
  },
});

export const foliageTools: ToolDef[] = [foliageInspect];
