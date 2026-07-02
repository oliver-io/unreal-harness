/**
 * Domain: class — read-only reflection over loaded UClasses.
 *
 * Port of the `class_*` tools in `src/MCP/server.py`. Both forward 1:1 to a
 * wire command equal to the tool name and operate over loaded classes only
 * (unloaded Blueprint classes are invisible). Read-only — PIE-safe.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const classQuery = bridgeTool({
  name: "class_query",
  domain: "class",
  description:
    "Find UClasses by name pattern and/or parent filter. Paginated, read-only. " +
    "Operates over LOADED UClasses only (unloaded Blueprint classes are invisible). " +
    "Hidden/HideDropDown/Deprecated filtered unless include_hidden=true.",
  input: z.object({
    name_pattern: z
      .string()
      .default("")
      .describe(
        "Case-insensitive substring match on the class short name. Empty matches everything (combine with parent to scope).",
      ),
    parent: z
      .string()
      .optional()
      .describe(
        'Optional parent class. FQN ("/Script/Engine.Actor"), prefixed short ("AActor"), or short ("Actor"). Filters via GetDerivedClasses.',
      ),
    recursive: z
      .boolean()
      .default(false)
      .describe("With parent, walk the full subclass tree. Default false (direct children only). Ignored without parent."),
    include_hidden: z
      .boolean()
      .default(false)
      .describe("Include CLASS_Hidden / CLASS_HideDropDown / CLASS_Deprecated."),
    cursor: z.number().int().default(0).describe("Page offset; pass next_cursor from a prior response. Default 0."),
    limit: z.number().int().default(200).describe("Page size, 1-1000. Default 200."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  // parent omitted when None (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = {
      name_pattern: a.name_pattern,
      recursive: a.recursive,
      include_hidden: a.include_hidden,
      cursor: a.cursor,
      limit: a.limit,
    };
    if (a.parent !== undefined) p.parent = a.parent;
    return p;
  },
});

const classInspect = bridgeTool({
  name: "class_inspect",
  domain: "class",
  description:
    "Inspect a UClass — properties, functions, and/or hierarchy in one call. Read-only. " +
    "include=['properties'|'functions'|'hierarchy'] (default ['properties']). " +
    "include_inherited walks the Super chain for properties/functions.",
  input: z.object({
    class_name: z
      .string()
      .min(1)
      .describe("UClass path or short name. Same flexible resolution as class_query (script path, asset path, short name with prefix fallback)."),
    include: z
      .array(z.string())
      .optional()
      .describe("Subset of ['properties', 'functions', 'hierarchy']. Default ['properties'] for back-compat. Pass any combination."),
    include_inherited: z
      .boolean()
      .default(false)
      .describe("When true, walks the Super chain for properties and functions. Default false (own members only). Hierarchy always includes the parent chain."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  // include omitted when None (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = {
      class_name: a.class_name,
      include_inherited: a.include_inherited,
    };
    if (a.include !== undefined) p.include = a.include;
    return p;
  },
});

export const classTools: ToolDef[] = [classQuery, classInspect];
