/**
 * Domain: mpc — Material Parameter Collection asset factory.
 *
 * Port of `mpc_create` in `src/MCP/server.py`. Wire command equals the tool
 * name. Asset mutator: refused during PIE and does not support dry_run.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

/** One initial MPC parameter entry; malformed/unknown entries are skipped server-side. */
const MpcParameter = z.object({
  type: z.string().describe('"scalar" or "vector".'),
  name: z.string().describe("Parameter name."),
  default_value: z
    .unknown()
    .optional()
    .describe('Scalar: a number. Vector: {"r","g","b","a"}.'),
});

const mpcCreate = bridgeTool({
  name: "mpc_create",
  domain: "mpc",
  description:
    "Create a new UMaterialParameterCollection (MPC) asset, optionally pre-seeded with " +
    "scalar/vector parameters. Auto-saves; path uniqueness enforced. Specify destination via " +
    "(path + name) or asset_path.",
  input: z.object({
    path: z.string().default("").describe("Destination package path. Pair with name."),
    name: z.string().default("").describe("Asset short name."),
    asset_path: z.string().default("").describe("Convenience combined path; replaces (path, name)."),
    parameters: z
      .array(MpcParameter)
      .optional()
      .describe(
        'Optional initial parameters. Each: {"type": "scalar"|"vector", "name", "default_value"}. ' +
          "Unknown/malformed entries are skipped silently.",
      ),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  // All keys omitted when empty/None (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.path) p.path = a.path;
    if (a.name) p.name = a.name;
    if (a.asset_path) p.asset_path = a.asset_path;
    if (a.parameters !== undefined) p.parameters = a.parameters;
    return p;
  },
});

export const mpcTools: ToolDef[] = [mpcCreate];
