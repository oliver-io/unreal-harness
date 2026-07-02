/**
 * Domain: eqs — Environment Query System query authoring and inspection.
 *
 * Port of the `eqs_*` tools in `src/MCP/server.py` (which forward through
 * `helpers/ai_eqs_runtime.py`). An EQS query asset holds ordered generator
 * *options*; each option owns a list of *tests*. Authoring mutators are blocked
 * during PIE; the add/remove mutators support dry_run (returns result.diff).
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";
import { dryRun } from "./_schemas.ts";

// Arbitrary property bag passed through to generator/test setters. apply
// silently ignores unrecognized names; dry_run surfaces a recognized/ignored
// split.
const properties = z
  .record(z.string(), z.unknown())
  .optional()
  .describe("Property name → value map for the generator/test. Unknown names are ignored on apply.");

const eqsCreate = bridgeTool({
  name: "eqs_create",
  domain: "eqs",
  description:
    "Create a new EQS (Environment Query System) query asset at asset_path. Blocked during PIE.",
  input: z.object({
    asset_path: z.string().min(1).describe("Package path for the new EnvQuery asset."),
  }),
  annotations: { blockedDuringPie: true },
});

const eqsRead = bridgeTool({
  name: "eqs_read",
  domain: "eqs",
  description:
    "Read the structure of an EQS query — options, generators, and tests with their properties.",
  input: z.object({
    asset_path: z.string().min(1).describe("Package path of the EnvQuery asset."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const eqsListTypes = bridgeTool({
  name: "eqs_list_types",
  domain: "eqs",
  description:
    "Discover registered EQS generator and test classes with their properties. " +
    'base_class filters the catalog ("all" returns both).',
  input: z.object({
    base_class: z
      .string()
      .default("all")
      .describe('Catalog filter: "all", or a base class to narrow generators/tests.'),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const eqsOptionAdd = bridgeTool({
  name: "eqs_option_add",
  domain: "eqs",
  description:
    'Add a generator option to an EQS query (e.g. "SimpleGrid", "OnCircle"). ' +
    "dry_run previews the would-be index, resolved class, and recognized/ignored property split. " +
    "Blocked during PIE.",
  input: z.object({
    asset_path: z.string().min(1).describe("Package path of the EnvQuery asset."),
    generator_type: z.string().min(1).describe('Generator type name, e.g. "SimpleGrid", "OnCircle".'),
    properties,
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
  // Wire dict from ai_eqs_runtime.add_eqs_option: properties omitted when falsy.
  params: (a) => {
    const p: Record<string, unknown> = {
      asset_path: a.asset_path,
      generator_type: a.generator_type,
      dry_run: a.dry_run,
    };
    if (a.properties !== undefined && Object.keys(a.properties).length > 0)
      p.properties = a.properties;
    return p;
  },
});

const eqsTestAdd = bridgeTool({
  name: "eqs_test_add",
  domain: "eqs",
  description:
    'Add a test to an EQS query option by index (e.g. "Distance", "Trace", "Dot"). ' +
    "dry_run previews the option_index, would-be test index, resolved class, and recognized/ignored " +
    "property split. Blocked during PIE.",
  input: z.object({
    asset_path: z.string().min(1).describe("Package path of the EnvQuery asset."),
    option_index: z.number().int().describe("Index of the option to add the test to."),
    test_type: z.string().min(1).describe('Test type name, e.g. "Distance", "Trace", "Dot".'),
    properties,
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
  // Wire dict from ai_eqs_runtime.add_eqs_test: properties omitted when falsy.
  params: (a) => {
    const p: Record<string, unknown> = {
      asset_path: a.asset_path,
      option_index: a.option_index,
      test_type: a.test_type,
      dry_run: a.dry_run,
    };
    if (a.properties !== undefined && Object.keys(a.properties).length > 0)
      p.properties = a.properties;
    return p;
  },
});

const eqsOptionRemove = bridgeTool({
  name: "eqs_option_remove",
  domain: "eqs",
  description:
    "Remove an option from an EQS query by index. dry_run previews the existing generator type " +
    "and the cascade of tests that disappear with it. Blocked during PIE.",
  input: z.object({
    asset_path: z.string().min(1).describe("Package path of the EnvQuery asset."),
    option_index: z.number().int().describe("Index of the option to remove."),
    dry_run: dryRun,
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
});

const eqsTestRemove = bridgeTool({
  name: "eqs_test_remove",
  domain: "eqs",
  description:
    "Remove a test from an EQS query option by index. dry_run previews the existing test class. " +
    "No cascade — tests are leaves. Blocked during PIE.",
  input: z.object({
    asset_path: z.string().min(1).describe("Package path of the EnvQuery asset."),
    option_index: z.number().int().describe("Index of the owning option."),
    test_index: z.number().int().describe("Index of the test within the option."),
    dry_run: dryRun,
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
});

const eqsSetProperty = bridgeTool({
  name: "eqs_set_property",
  domain: "eqs",
  description:
    'Set a property on an EQS generator or test. target is the literal "generator" ' +
    '(case-insensitive) or a numeric test-index string such as "0"; any other string is rejected ' +
    "with invalid_argument. Blocked during PIE.",
  input: z.object({
    asset_path: z.string().min(1).describe("Package path of the EnvQuery asset."),
    option_index: z.number().int().describe("Index of the option owning the generator/test."),
    target: z
      .string()
      .min(1)
      .describe('"generator" (case-insensitive) or a numeric test-index string like "0".'),
    property_name: z.string().min(1).describe("Name of the property to set."),
    property_value: z.unknown().describe("New value for the property."),
  }),
  annotations: { idempotentHint: true, blockedDuringPie: true },
});

export const eqsTools: ToolDef[] = [
  eqsCreate,
  eqsRead,
  eqsListTypes,
  eqsOptionAdd,
  eqsTestAdd,
  eqsOptionRemove,
  eqsTestRemove,
  eqsSetProperty,
];
