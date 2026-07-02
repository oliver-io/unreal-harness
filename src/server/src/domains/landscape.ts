/**
 * Domain: landscape — read-only landscape inspection (GAPS #13).
 *
 * Mutation (sculpt / paint / heightmap import) is refused by design — that is
 * brush-driven content authoring and belongs to the editor's Landscape mode.
 * These tools only read existing landscape state. Wire commands equal the tool
 * names. All read-only; none PIE- or dry-run-gated.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const landscapeInspect = bridgeTool({
  name: "landscape_inspect",
  domain: "landscape",
  description:
    "Enumerate landscape actors in the editor world and their core properties (read-only). " +
    "Returns landscapes[] with name, class (Landscape / LandscapeStreamingProxy), guid, " +
    "location, scale, component_size_quads, subsection_size_quads, num_subsections, material, " +
    "extent_quads {min/max/width/height}, and paint_layer_count. Omit actor_name to list all.",
  input: z.object({
    actor_name: z
      .string()
      .default("")
      .describe("Optional landscape actor label/name to inspect; empty lists every landscape."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => (a.actor_name ? { actor_name: a.actor_name } : {}),
});

const landscapeListLayers = bridgeTool({
  name: "landscape_list_layers",
  domain: "landscape",
  description:
    "List a landscape's paint (target) layers (read-only). Returns layers[] with name, " +
    "layer_info_object (asset path, or empty if unassigned), and assigned (bool). Omit " +
    "actor_name to use the first landscape in the world.",
  input: z.object({
    actor_name: z
      .string()
      .default("")
      .describe("Landscape actor label/name; empty uses the first landscape in the world."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => (a.actor_name ? { actor_name: a.actor_name } : {}),
});

const landscapeReadHeightmap = bridgeTool({
  name: "landscape_read_heightmap",
  domain: "landscape",
  description:
    "Read a bounded summary of a landscape's heightmap (read-only, editor-only). Returns the " +
    "resolved region, samples_read, and height_stats (min/max/mean raw uint16 + min/max world Z). " +
    "Never dumps a full grid inline: large regions must be narrowed with region or written to " +
    "export_path. Set include_samples to also return a small inline grid (<=256x256). " +
    "Pass export_path (.r16) to write the raw row-major uint16 grid to disk.",
  input: z.object({
    actor_name: z
      .string()
      .default("")
      .describe("Landscape actor label/name; empty uses the first landscape in the world."),
    region: z
      .object({
        min_x: z.number().int(),
        min_y: z.number().int(),
        max_x: z.number().int(),
        max_y: z.number().int(),
      })
      .partial()
      .optional()
      .describe("Optional quad-space sub-region (clamped to the landscape extent)."),
    export_path: z
      .string()
      .default("")
      .describe("Optional absolute filesystem path to write the raw .r16 heightmap grid."),
    include_samples: z
      .boolean()
      .default(false)
      .describe("If true and the region is <=256x256, include an inline row-major samples[] grid."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.actor_name) p.actor_name = a.actor_name;
    if (a.region) p.region = a.region;
    if (a.export_path) p.export_path = a.export_path;
    if (a.include_samples) p.include_samples = a.include_samples;
    return p;
  },
});

export const landscapeTools: ToolDef[] = [
  landscapeInspect,
  landscapeListLayers,
  landscapeReadHeightmap,
];
