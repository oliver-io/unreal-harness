/**
 * Domain: material — Material / MaterialInstance / MaterialFunction assets and
 * the material expression graph.
 *
 * Port of the `material_*` tools in `src/MCP/server.py`. Mutators are blocked
 * during PIE (mirrors `IsBlockedDuringPie` in MCPCommonUtils.cpp);
 * `material_function_create` additionally refuses dry_run (`IsBlockedFromDryRun`).
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool, defineTool } from "./_shared.ts";
import { dryRun } from "./_schemas.ts";

/** r/g/b/a may be a color component (number) OR a ComponentMask toggle (bool). */
const rgbaComponent = z.union([z.boolean(), z.number()]);

const materialFunctionCreate = bridgeTool({
  name: "material_function_create",
  domain: "material",
  description:
    "Create a new UMaterialFunction asset (empty graph). Destination via path/name " +
    "or asset_path. expose_to_library publishes it to the global function library. " +
    "Add expressions afterwards via material_add_expression. dry_run unsupported.",
  input: z.object({
    path: z.string().default("").describe("Destination package folder."),
    name: z.string().default("").describe("Asset name."),
    asset_path: z.string().default("").describe("Full destination path (alt to path/name)."),
    description: z.string().default("").describe("Human-facing description in the function picker."),
    expose_to_library: z
      .boolean()
      .default(false)
      .describe("When true, the function appears in the global material function library."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = { expose_to_library: a.expose_to_library };
    if (a.path) p.path = a.path;
    if (a.name) p.name = a.name;
    if (a.asset_path) p.asset_path = a.asset_path;
    if (a.description) p.description = a.description;
    return p;
  },
});

const materialGetAvailable = bridgeTool({
  name: "material_get_available",
  domain: "material",
  description:
    "List available materials in the project that can be applied to objects. " +
    "Returns count + materials[]. include_engine_materials adds engine content.",
  input: z.object({
    search_path: z.string().default("/Game/").describe("Content path to search."),
    include_engine_materials: z.boolean().default(true).describe("Include /Engine materials."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const materialApplyToActor = bridgeTool({
  name: "material_apply_to_actor",
  domain: "material",
  description:
    "Apply a material to a placed actor at a given slot index. By default it paints the slot on " +
    "EVERY static-mesh component of the actor (correct for single-mesh actors). On a multi-mesh " +
    "actor, pass component_name to target ONE named component. The result echoes " +
    "components_affected[] so a broad apply is never mistaken for a targeted one.",
  input: z.object({
    actor_name: z.string().min(1).describe("Target actor name."),
    material_path: z.string().min(1).describe("Path to the UMaterialInterface to assign."),
    material_slot: z.number().int().default(0).describe("Material slot index (default 0)."),
    component_name: z
      .string()
      .min(1)
      .optional()
      .describe(
        "Optional: apply to ONLY this static-mesh component (matched by its UObject name, e.g. " +
          "'GoalBlue'). Omit to apply to all mesh components. Use actor_inspect / " +
          "get_actor_material_info to enumerate component names.",
      ),
  }),
  annotations: { blockedDuringPie: true },
});

const materialApplyToBlueprint = bridgeTool({
  name: "material_apply_to_blueprint",
  domain: "material",
  description: "Apply a material to a named component inside a Blueprint at a given slot index.",
  input: z.object({
    blueprint_name: z.string().min(1).describe("Target Blueprint name."),
    component_name: z.string().min(1).describe("Component within the Blueprint."),
    material_path: z.string().min(1).describe("Path to the UMaterialInterface to assign."),
    material_slot: z.number().int().default(0).describe("Material slot index (default 0)."),
  }),
  annotations: { blockedDuringPie: true },
});

const materialCompile = bridgeTool({
  name: "material_compile",
  domain: "material",
  description:
    "Compile a Material and return any shader compilation errors. success=true when " +
    "no errors, else success=false with an errors[] array.",
  input: z.object({
    material_path: z.string().min(1).describe('Full path to the Material asset (e.g. "/Game/Materials/M_Stone").'),
  }),
  annotations: { blockedDuringPie: true },
});

const materialRead = bridgeTool({
  name: "material_read",
  domain: "material",
  description:
    "Read the full expression graph of a Material: expressions (nodes), connections, " +
    "referenced textures, parameters, and final output assignments (BaseColor, Normal, …).",
  input: z.object({
    material_path: z.string().min(1).describe('Full path to the Material asset (e.g. "/Game/Materials/M_Stone.M_Stone").'),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const materialReadFunction = bridgeTool({
  name: "material_read_function",
  domain: "material",
  description:
    "Read the expression graph of a MaterialFunction: expressions, connections, and the " +
    "function's declared inputs/outputs. Same node structure as material_read.",
  input: z.object({
    function_path: z
      .string()
      .min(1)
      .describe('Full path to the MaterialFunction asset (e.g. "/Game/Materials/MF_MyFunc.MF_MyFunc").'),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const materialReadInstance = bridgeTool({
  name: "material_read_instance",
  domain: "material",
  description:
    "Read a Material Instance (MIC): parent chain plus scalar/vector/texture/static-switch " +
    "parameter overrides. Per-parameter keys round-trip with material_instance_set_parameter.",
  input: z.object({
    instance_path: z.string().min(1).describe('Full path to the Material Instance asset (e.g. "/Game/Materials/MI_MyInstance").'),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const materialInstanceSetParameter = defineTool({
  name: "material_instance_set_parameter",
  domain: "material",
  description:
    "Set a parameter override on a Material Instance (MIC). parameter_type is one of " +
    "scalar | vector | texture | static_switch; supply the matching fields (value for " +
    "scalar/static_switch, r/g/b/a for vector, texture_path for texture). Supports dry_run.",
  input: z.object({
    instance_path: z.string().min(1).describe("Full path to the Material Instance."),
    parameter_name: z.string().min(1).describe("Name of the parameter to set."),
    parameter_type: z
      .enum(["scalar", "vector", "texture", "static_switch"])
      .describe("scalar | vector | texture | static_switch."),
    value: z.number().optional().describe("Scalar value, or bool (0/1) for static_switch."),
    r: z.number().optional().describe("Red channel for vector params (0-1)."),
    g: z.number().optional().describe("Green channel for vector params (0-1)."),
    b: z.number().optional().describe("Blue channel for vector params (0-1)."),
    a: z.number().default(1.0).describe("Alpha channel for vector params (0-1, default 1.0)."),
    texture_path: z.string().optional().describe("Texture asset path for texture params (alias: texture)."),
    override: z
      .boolean()
      .optional()
      .describe("static_switch only: flip bOverride (true) or clear it (false)."),
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
  handler: (a, ctx) => {
    const params: Record<string, unknown> = {
      instance_path: a.instance_path,
      parameter_name: a.parameter_name,
      parameter_type: a.parameter_type,
      dry_run: a.dry_run,
    };
    if (a.parameter_type === "scalar") {
      if (a.value === undefined || a.value === null)
        return { success: false, message: "scalar type requires 'value'" };
      params.value = a.value;
    } else if (a.parameter_type === "vector") {
      params.r = a.r ?? 0;
      params.g = a.g ?? 0;
      params.b = a.b ?? 0;
      params.a = a.a;
    } else if (a.parameter_type === "texture") {
      if (!a.texture_path)
        return { success: false, message: "texture type requires 'texture_path'" };
      params.texture_path = a.texture_path;
    } else if (a.parameter_type === "static_switch") {
      if (a.value === undefined || a.value === null)
        return { success: false, message: "static_switch type requires 'value' (true/false)" };
      params.value = Boolean(a.value);
      if (a.override !== undefined) params.override = Boolean(a.override);
    }
    return ctx.conn.sendCommand("material_instance_set_parameter", params);
  },
});

const materialReparentInstance = bridgeTool({
  name: "material_reparent_instance",
  domain: "material",
  description:
    "Change the parent of a Material Instance. Overrides absent from the new parent become " +
    "orphaned.",
  input: z.object({
    instance_path: z.string().min(1).describe("Full path to the Material Instance to reparent."),
    new_parent_path: z.string().min(1).describe("Full path to the new parent Material or MaterialInstance."),
  }),
  annotations: { blockedDuringPie: true },
});

const materialCreate = defineTool({
  name: "material_create",
  domain: "material",
  description:
    "Create a new UMaterial asset on disk. material_path's last component is the asset name. " +
    "Optional top-level props (blend_mode, two_sided, shading_model, material_domain) apply " +
    "before the initial compile. UMG/Slate widget materials MUST use material_domain='UI'.",
  input: z.object({
    material_path: z.string().min(1).describe('Destination asset path, e.g. "/Game/Materials/M_Liquid".'),
    blend_mode: z
      .string()
      .optional()
      .describe("Opaque | Masked | Translucent | Additive | Modulate | AlphaComposite | AlphaHoldout."),
    two_sided: z.boolean().optional().describe("If true, render both faces."),
    shading_model: z
      .string()
      .optional()
      .describe("DefaultLit | Unlit | Subsurface | ClearCoat | … (case-insensitive; Lit==DefaultLit)."),
    material_domain: z
      .string()
      .optional()
      .describe("Surface | UI | PostProcess | DeferredDecal | LightFunction | Volume."),
  }),
  annotations: { blockedDuringPie: true },
  handler: (a, ctx) => {
    const normalized = a.material_path.replace(/\/+$/, "");
    if (!normalized.includes("/"))
      return {
        success: false,
        message: `material_path must include a package directory, e.g. '/Game/Materials/M_Liquid' (got '${a.material_path}')`,
      };
    const idx = normalized.lastIndexOf("/");
    const package_path = normalized.slice(0, idx);
    const material_name = normalized.slice(idx + 1);
    if (!package_path || !material_name)
      return {
        success: false,
        message: `Could not split material_path into (package_path, material_name): '${a.material_path}'`,
      };
    const params: Record<string, unknown> = {
      package_path,
      material_name,
      material_path: normalized,
    };
    if (a.blend_mode !== undefined) params.blend_mode = a.blend_mode;
    if (a.two_sided !== undefined) params.two_sided = a.two_sided;
    if (a.shading_model !== undefined) params.shading_model = a.shading_model;
    if (a.material_domain !== undefined) params.material_domain = a.material_domain;
    return ctx.conn.sendCommand("material_create", params);
  },
});

const materialSetProperty = bridgeTool({
  name: "material_set_property",
  domain: "material",
  description:
    "Change top-level flags on an EXISTING UMaterial (not an instance): blend_mode, two_sided, " +
    "shading_model, material_domain. material_create only set these at creation; this flips them " +
    "afterward (e.g. Translucent→Opaque) and recompiles + saves. Pass at least one field.",
  input: z.object({
    material_path: z.string().min(1).describe("Asset path of an existing UMaterial."),
    blend_mode: z
      .string()
      .optional()
      .describe("Opaque | Masked | Translucent | Additive | Modulate | AlphaComposite | AlphaHoldout."),
    two_sided: z.boolean().optional().describe("If true, render both faces."),
    shading_model: z
      .string()
      .optional()
      .describe("DefaultLit | Unlit | Subsurface | ClearCoat | … (case-insensitive; Lit==DefaultLit)."),
    material_domain: z
      .string()
      .optional()
      .describe("Surface | UI | PostProcess | DeferredDecal | LightFunction | Volume."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = { material_path: a.material_path };
    if (a.blend_mode !== undefined) p.blend_mode = a.blend_mode;
    if (a.two_sided !== undefined) p.two_sided = a.two_sided;
    if (a.shading_model !== undefined) p.shading_model = a.shading_model;
    if (a.material_domain !== undefined) p.material_domain = a.material_domain;
    return p;
  },
});

const materialCreateInstance = bridgeTool({
  name: "material_create_instance",
  domain: "material",
  description:
    "Create a UMaterialInstanceConstant whose Parent is a given UMaterial(Interface). No " +
    "parameters set — wire them in afterwards via material_instance_set_parameter.",
  input: z.object({
    asset_path: z.string().min(1).describe('Destination asset path, e.g. "/Game/Generated/MI_RedClay".'),
    parent_material: z.string().min(1).describe("Path of the parent UMaterial(Interface). Must already exist."),
    force_overwrite: z.boolean().default(false).describe("Replace any existing asset at asset_path (default false)."),
  }),
  annotations: { blockedDuringPie: true },
});

const materialAddExpression = bridgeTool({
  name: "material_add_expression",
  domain: "material",
  description:
    "Add a material expression node to a Material's graph. expression_type is the UE class " +
    "name without the 'MaterialExpression' prefix (Constant3Vector, TextureSample, Multiply, …); " +
    "supply only the per-type fields that node needs. Wire pins via material_connect. Supports dry_run.",
  input: z.object({
    material_path: z.string().min(1).describe('Material asset path, e.g. "/Game/Materials/M_Liquid".'),
    expression_type: z.string().min(1).describe('UE class name without the "MaterialExpression" prefix.'),
    position_x: z.number().default(0.0).describe("Editor canvas X position."),
    position_y: z.number().default(0.0).describe("Editor canvas Y position."),
    desc: z.string().optional().describe("Optional node description / comment."),
    r: rgbaComponent.optional().describe("Color component or ComponentMask toggle."),
    g: rgbaComponent.optional().describe("Color component or ComponentMask toggle."),
    b: rgbaComponent.optional().describe("Color component or ComponentMask toggle."),
    a: rgbaComponent.optional().describe("Color component or ComponentMask toggle."),
    parameter_name: z.string().optional().describe("Parameter name for *Parameter expression types."),
    value: z.number().optional().describe("Constant scalar value."),
    default_value: z.number().optional().describe("ScalarParameter default."),
    default_r: z.number().optional().describe("VectorParameter long-form red."),
    default_g: z.number().optional().describe("VectorParameter long-form green."),
    default_b: z.number().optional().describe("VectorParameter long-form blue."),
    default_a: z.number().optional().describe("VectorParameter long-form alpha."),
    texture_path: z.string().optional().describe("Texture asset path for sample expressions (alias: texture)."),
    u_tiling: z.number().optional().describe("TextureCoordinate U tiling."),
    v_tiling: z.number().optional().describe("TextureCoordinate V tiling."),
    coordinate_index: z.number().int().optional().describe("TextureCoordinate coordinate index."),
    layers: z.array(z.record(z.string(), z.unknown())).optional().describe("LandscapeLayerBlend layer specs (create-only)."),
    period: z.number().optional().describe("Time period in seconds."),
    ignore_pause: z.boolean().optional().describe("Time: ignore game pause."),
    equals_threshold: z.number().optional().describe("If: equals threshold (default engine CDO)."),
    code: z.string().optional().describe("Custom: HLSL body."),
    output_type: z.string().optional().describe('Custom: "Float1".."Float4".'),
    inputs: z.array(z.record(z.string(), z.unknown())).optional().describe("Custom: input pin specs [{name}]."),
    param_names: z.array(z.string()).optional().describe("DynamicParameter: 4 RGBA output labels."),
    parameter_index: z.number().int().optional().describe("DynamicParameter: channel 0-3."),
    scene_texture_id: z
      .string()
      .optional()
      .describe(
        'SceneTexture buffer: "PostProcessInput0" (default), "SceneColor", "SceneDepth", "Velocity", ' +
          '"WorldNormal", "CustomDepth", "CustomStencil", "BaseColor", "WorldTangent", "AmbientOcclusion".',
      ),
    filtered: z.boolean().optional().describe("SceneTexture: bilinear filtering of the lookup."),
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      material_path: a.material_path,
      expression_type: a.expression_type,
      pos_x: a.position_x,
      pos_y: a.position_y,
      position_x: a.position_x,
      position_y: a.position_y,
      dry_run: a.dry_run,
    };
    const optional: Record<string, unknown> = {
      desc: a.desc,
      r: a.r, g: a.g, b: a.b, a: a.a,
      parameter_name: a.parameter_name,
      value: a.value,
      default_value: a.default_value,
      default_r: a.default_r, default_g: a.default_g,
      default_b: a.default_b, default_a: a.default_a,
      texture_path: a.texture_path,
      u_tiling: a.u_tiling, v_tiling: a.v_tiling,
      coordinate_index: a.coordinate_index,
      layers: a.layers,
      period: a.period,
      ignore_pause: a.ignore_pause,
      equals_threshold: a.equals_threshold,
      code: a.code,
      output_type: a.output_type,
      inputs: a.inputs,
      param_names: a.param_names,
      parameter_index: a.parameter_index,
      scene_texture_id: a.scene_texture_id,
      filtered: a.filtered,
    };
    for (const [key, val] of Object.entries(optional)) {
      if (val !== undefined) p[key] = val;
    }
    return p;
  },
});

const materialSetExpressionProperty = bridgeTool({
  name: "material_set_expression_property",
  domain: "material",
  description:
    "Mutate properties of an existing material expression node (subset of " +
    "material_add_expression's fields; no layers). WARNING: multi-component color/vector " +
    "setters ZERO-FILL omitted components — supply all of r/g/b/a to preserve them. Supports dry_run.",
  input: z.object({
    material_path: z.string().min(1).describe("Material asset path."),
    expression_name: z.string().min(1).describe("Name of the existing expression to mutate."),
    position_x: z.number().optional().describe("Editor canvas X position."),
    position_y: z.number().optional().describe("Editor canvas Y position."),
    desc: z.string().optional().describe("Node description / comment."),
    r: rgbaComponent.optional().describe("Color component or ComponentMask toggle."),
    g: rgbaComponent.optional().describe("Color component or ComponentMask toggle."),
    b: rgbaComponent.optional().describe("Color component or ComponentMask toggle."),
    a: rgbaComponent.optional().describe("Color component or ComponentMask toggle."),
    parameter_name: z.string().optional().describe("Parameter name for *Parameter expression types."),
    value: z.number().optional().describe("Constant scalar value."),
    default_value: z.number().optional().describe("ScalarParameter default."),
    default_r: z.number().optional().describe("VectorParameter long-form red."),
    default_g: z.number().optional().describe("VectorParameter long-form green."),
    default_b: z.number().optional().describe("VectorParameter long-form blue."),
    default_a: z.number().optional().describe("VectorParameter long-form alpha."),
    texture_path: z.string().optional().describe("Texture asset path for sample expressions."),
    u_tiling: z.number().optional().describe("TextureCoordinate U tiling."),
    v_tiling: z.number().optional().describe("TextureCoordinate V tiling."),
    coordinate_index: z.number().int().optional().describe("TextureCoordinate coordinate index."),
    code: z.string().optional().describe("Custom node: HLSL body (edit in place — no need to delete+re-add)."),
    output_type: z
      .string()
      .optional()
      .describe("Custom node: Float1/Float2/Float3/Float4 output type."),
    inputs: z
      .array(z.object({ name: z.string() }))
      .optional()
      .describe("Custom node: input pin definitions (rebuilds the node's input pins)."),
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      material_path: a.material_path,
      expression_name: a.expression_name,
      dry_run: a.dry_run,
    };
    const optional: Record<string, unknown> = {
      pos_x: a.position_x, pos_y: a.position_y,
      position_x: a.position_x, position_y: a.position_y,
      desc: a.desc,
      r: a.r, g: a.g, b: a.b, a: a.a,
      parameter_name: a.parameter_name,
      value: a.value, default_value: a.default_value,
      default_r: a.default_r, default_g: a.default_g,
      default_b: a.default_b, default_a: a.default_a,
      texture_path: a.texture_path,
      u_tiling: a.u_tiling, v_tiling: a.v_tiling,
      coordinate_index: a.coordinate_index,
      code: a.code, output_type: a.output_type, inputs: a.inputs,
    };
    for (const [key, val] of Object.entries(optional)) {
      if (val !== undefined) p[key] = val;
    }
    return p;
  },
});

const materialDeleteExpression = bridgeTool({
  name: "material_delete_expression",
  domain: "material",
  description:
    "Remove an expression node from a Material's graph. Connections to/from it are dropped " +
    "(referencing wires go null). Supports dry_run (enumerates severed connections).",
  input: z.object({
    material_path: z.string().min(1).describe("Material asset path."),
    expression_name: z.string().min(1).describe("Name of the expression to delete."),
    dry_run: dryRun,
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
});

const materialConnect = bridgeTool({
  name: "material_connect",
  domain: "material",
  description:
    "Wire a source expression's output to another expression's input pin, or to one of the " +
    "Material's main inputs (target_expression='Material', target_input=BaseColor/Normal/…). " +
    "Connecting a non-empty slot OVERWRITES it. Supports dry_run.",
  input: z.object({
    material_path: z.string().min(1).describe("Material asset path."),
    source_expression: z.string().min(1).describe("Source expression name (numeric = collection index)."),
    target_input: z.string().min(1).describe("Target input pin / material attribute name (or numeric index)."),
    target_expression: z
      .string()
      .default("Material")
      .describe("'Material' for a main input (default), else an existing expression name."),
    source_output_index: z.number().int().default(0).describe("Output pin index on the source (default 0)."),
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
});

export const materialTools: ToolDef[] = [
  materialAddExpression,
  materialApplyToActor,
  materialApplyToBlueprint,
  materialCompile,
  materialConnect,
  materialCreate,
  materialCreateInstance,
  materialDeleteExpression,
  materialFunctionCreate,
  materialGetAvailable,
  materialInstanceSetParameter,
  materialRead,
  materialReadFunction,
  materialReadInstance,
  materialReparentInstance,
  materialSetProperty,
  materialSetExpressionProperty,
];
