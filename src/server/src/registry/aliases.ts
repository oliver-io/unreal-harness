/**
 * Concept-level parameter aliases (GAP-021 / GAP-041).
 *
 * The same concept is keyed differently across the tool surface — a Blueprint
 * identifier is `blueprint_path` on `bp_read`/`bp_inspect`, `blueprint_name` on
 * `bp_compile`/`bp_add_node`, and `bp_path` on `bp_brief`; a material is
 * `material_path`, an arbitrary asset is `asset_path`. Each tool's Zod object
 * hard-declared ONE literal key, so passing a sibling spelling was a
 * `validation_error`.
 *
 * Rather than rename params (which would break existing callers), an affected
 * tool ACCEPTS its sibling spellings and normalizes them to its canonical key
 * before the handler runs. The mechanism is two-sided:
 *
 *   1. {@link augmentInput} widens the tool's ZodObject so the alias keys are
 *      ACCEPTED (added as optional) and the canonical key is RELAXED to optional
 *      (it can now be satisfied via an alias). It stays a `ZodObject`, so
 *      `def.input.shape` (the SDK's validator) and `def.input.safeParse`
 *      (catalog_call / code-mode) both keep working — no generic-bound change.
 *   2. {@link applyAliases} runs after validation, at every handler-dispatch
 *      site, moving any alias-keyed value onto its canonical key and dropping
 *      the alias key — so the bridge/handler only ever sees the canonical name.
 *
 * Scope is deliberately conservative — three documented concepts only, no
 * sprawling dictionary:
 *   - Blueprint identifier (`bp` domain): blueprint_path == blueprint_name == bp_path
 *   - Material asset path (`material` domain): material_path also accepts `path`, `asset_path`
 *   - Asset package path (`asset` domain): asset_path also accepts `path`
 */

import { z } from "zod";
import type { ToolDef } from "./types.ts";

/** Map of alias key → canonical key, for a single tool. */
export type AliasMap = Record<string, string>;

/** The three interchangeable spellings of a Blueprint identifier. */
const BP_ID_KEYS = ["blueprint_path", "blueprint_name", "bp_path"] as const;

/**
 * Move any alias-keyed value onto its canonical key and drop the alias keys.
 * Pure; returns `args` unchanged (same reference) when there is nothing to do —
 * so the 230+ tools without aliases pay nothing. The canonical key wins if BOTH
 * it and an alias are present (the alias is simply discarded).
 */
export function applyAliases(
  args: Record<string, unknown>,
  aliases: AliasMap | undefined,
): Record<string, unknown> {
  if (!aliases) return args;
  let out: Record<string, unknown> | undefined;
  for (const [alias, canonical] of Object.entries(aliases)) {
    if (!(alias in args)) continue;
    out ??= { ...args };
    if (out[canonical] === undefined && out[alias] !== undefined) {
      out[canonical] = out[alias];
    }
    delete out[alias];
  }
  return out ?? args;
}

/**
 * Return a ZodObject that additionally accepts the alias keys (optional) and
 * relaxes each aliased canonical key to optional. Existing real params are never
 * overridden (an alias whose key already exists on the tool is skipped upstream).
 */
function augmentInput(
  input: z.ZodObject<z.ZodRawShape>,
  aliases: AliasMap,
): z.ZodObject<z.ZodRawShape> {
  const shape = input.shape;
  const ext: z.ZodRawShape = {};
  // Relax each aliased canonical to optional (satisfiable via an alias).
  for (const canonical of new Set(Object.values(aliases))) {
    const field = shape[canonical];
    if (field) ext[canonical] = (field as z.ZodTypeAny).optional();
  }
  // Add each alias as an optional clone of its canonical's schema.
  for (const [alias, canonical] of Object.entries(aliases)) {
    const field = shape[canonical];
    if (field) {
      ext[alias] = (field as z.ZodTypeAny)
        .optional()
        .describe(`Alias for ${canonical}.`);
    }
  }
  return input.extend(ext);
}

/** Attach an alias map to one def: augment its input + record the map. */
function attach(def: ToolDef, aliases: AliasMap): ToolDef {
  if (Object.keys(aliases).length === 0) return def;
  return { ...def, input: augmentInput(def.input, aliases), aliases };
}

/**
 * `bp` domain: within this domain the three BP_ID_KEYS unambiguously denote the
 * Blueprint identifier, so whichever one a tool uses canonically, accept the
 * other two. A tool with zero (or, defensively, more than one) of the keys is
 * left untouched.
 */
export function withBpAliases(defs: readonly ToolDef[]): ToolDef[] {
  return defs.map((def) => {
    const present = BP_ID_KEYS.filter((k) => k in def.input.shape);
    if (present.length !== 1) return def;
    const canonical = present[0]!;
    const aliases: AliasMap = {};
    for (const k of BP_ID_KEYS) if (k !== canonical) aliases[k] = canonical;
    return attach(def, aliases);
  });
}

/** `material` domain: a tool keyed on `material_path` also accepts `path`/`asset_path`. */
export function withMaterialAliases(defs: readonly ToolDef[]): ToolDef[] {
  return defs.map((def) => {
    const shape = def.input.shape;
    if (!("material_path" in shape)) return def;
    const aliases: AliasMap = {};
    if (!("path" in shape)) aliases.path = "material_path";
    if (!("asset_path" in shape)) aliases.asset_path = "material_path";
    return attach(def, aliases);
  });
}

/** `asset` domain: a tool keyed on `asset_path` also accepts the generic `path`. */
export function withAssetAliases(defs: readonly ToolDef[]): ToolDef[] {
  return defs.map((def) => {
    const shape = def.input.shape;
    if (!("asset_path" in shape)) return def;
    if ("path" in shape) return def; // never shadow a real `path` param
    return attach(def, { path: "asset_path" });
  });
}
