/**
 * Domain: enum — UserDefinedEnum asset creation + UEnum reflection inspection.
 *
 * Port of the `enum_*` tools in `src/MCP/server.py`. Wire commands equal the tool
 * names. enum_inspect is a PIE-safe read; enum_create is an asset-factory mutator
 * (refused during PIE, no dry_run support).
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const enumInspect = bridgeTool({
  name: "enum_inspect",
  domain: "enum",
  description:
    "Inspect a UEnum — members, display names, integer values, metadata flags. Read-only, live " +
    "reflection data. Accepts native enums (/Script/Engine.EBlendMode, EBlendMode, BlendMode) and " +
    "UserDefinedEnum assets (/Game/Path/UDE_Foo). Returns enum_name, enum_path, cpp_form, " +
    "is_user_defined, member_count, members[] (the synthetic _MAX sentinel is excluded).",
  input: z.object({
    enum_name: z
      .string()
      .describe(
        'Fully-qualified path, asset path, or short name. E.g. "/Script/Engine.EBlendMode", ' +
          '"EBlendMode", "BlendMode" (E-prefix added as fallback), or "/Game/Data/Enums/UDE_Faction".',
      ),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

/** A member is a display-name string or {display_name}. */
const EnumMember = z.union([
  z.string(),
  z.object({ display_name: z.string() }),
]);

const enumCreate = bridgeTool({
  name: "enum_create",
  domain: "enum",
  description:
    "Create a new UserDefinedEnum (UUserDefinedEnum) asset. Auto-saves on success; path uniqueness " +
    "enforced (name_collision on existing path, no silent overwrite). Specify (path, name) OR a " +
    "combined asset_path. No dry_run support.",
  input: z.object({
    path: z
      .string()
      .default("")
      .describe('Destination package path ("/Game/Data/Enums"). Pair with `name`.'),
    name: z.string().default("").describe('Asset short name ("EFoo"). Pair with `path`.'),
    asset_path: z
      .string()
      .default("")
      .describe('Convenience — combined "/Game/Data/Enums/EFoo" replaces (path, name).'),
    members: z
      .array(EnumMember)
      .optional()
      .describe(
        'Optional initial members. Each entry is a display-name string or {"display_name": "..."}. ' +
          "The first entry replaces the auto-created NewEnumerator0; the rest are appended. Values are " +
          "sequential (0..N-1).",
      ),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  // Omit-when-empty/None to mirror the Python param dict exactly.
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.path) p.path = a.path;
    if (a.name) p.name = a.name;
    if (a.asset_path) p.asset_path = a.asset_path;
    if (a.members !== undefined) p.members = a.members;
    return p;
  },
});

export const enumTools: ToolDef[] = [enumInspect, enumCreate];
