/**
 * Domain: reflection — native UCLASS reflection reads.
 *
 * Port of `reflection_class_properties` in `src/MCP/server.py`. Wire command
 * equals the tool name. Read-only.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const reflectionClassProperties = bridgeTool({
  name: "reflection_class_properties",
  domain: "reflection",
  description:
    "Read C++ UCLASS reflection and return its UPROPERTYs with full type info. Bridges native " +
    "(C++-declared) parent-class properties that get_blueprint_variable_details cannot see. " +
    "Returns properties[] with type, category, default_value, replication/visibility flags, declared_in.",
  input: z.object({
    class_name: z
      .string()
      .min(1)
      .describe(
        'Class to introspect: FQN ("/Script/MyGame.MyAnimInstance"), U-prefixed short name ' +
          '("UForestAnimalAnimInstance"), or bare short name ("ForestAnimalAnimInstance").',
      ),
    include_inherited: z
      .boolean()
      .default(false)
      .describe("Walk the Super chain to also report parent properties. Default false — own UPROPERTYs only."),
    property_name: z
      .string()
      .optional()
      .describe("Optional — return just this one property (mirrors get_blueprint_variable_details semantics)."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  // class_name + include_inherited always sent; property_name omitted when None (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = {
      class_name: a.class_name,
      include_inherited: a.include_inherited,
    };
    if (a.property_name !== undefined) p.property_name = a.property_name;
    return p;
  },
});

export const reflectionTools: ToolDef[] = [reflectionClassProperties];
