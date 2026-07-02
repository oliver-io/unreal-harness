/**
 * Domain: niagara — Niagara System/emitter/module authoring and inspection.
 *
 * Port of the `niagara_*` tools in `src/MCP/server.py`. Read tools inspect an
 * existing system; the create/structural tools build one from scratch. Wire
 * command names equal the tool names (verified against the C++ dispatch).
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

/** Optional emitter selector — provide name OR zero-based index. */
const emitterSelector = {
  emitter_name: z
    .string()
    .optional()
    .describe("Emitter name. Provide this or emitter_index."),
  emitter_index: z
    .number()
    .int()
    .optional()
    .describe("Zero-based emitter index. Provide this or emitter_name."),
};

/** Optional scalar/vector/color components shared by the two value setters. */
const valueComponents = {
  value: z
    .number()
    .optional()
    .describe("Scalar value for float, int32, or bool parameters."),
  x: z.number().optional().describe("Component for vector parameters."),
  y: z.number().optional().describe("Component for vector parameters."),
  z: z.number().optional().describe("Component for vector parameters."),
  w: z.number().optional().describe("Component for vector4 parameters."),
  r: z.number().optional().describe("Component for linear_color parameters."),
  g: z.number().optional().describe("Component for linear_color parameters."),
  b: z.number().optional().describe("Component for linear_color parameters."),
  a: z.number().optional().describe("Component for linear_color parameters."),
};

/** Append emitter selector fields to wire params, omitting unset ones. */
function addEmitter(
  p: Record<string, unknown>,
  a: { emitter_name?: string; emitter_index?: number },
): void {
  if (a.emitter_name !== undefined) p.emitter_name = a.emitter_name;
  if (a.emitter_index !== undefined) p.emitter_index = a.emitter_index;
}

/** Append present value-components to wire params (Python: `if v is not None`). */
function addComponents(
  p: Record<string, unknown>,
  a: Record<string, unknown>,
): void {
  for (const k of ["value", "x", "y", "z", "w", "r", "g", "b", "a"] as const) {
    if (a[k] !== undefined) p[k] = a[k];
  }
}

// ── Inspection ────────────────────────────────────────────────────────────

const niagaraListSystems = bridgeTool({
  name: "niagara_list_systems",
  domain: "niagara",
  description:
    "List all Niagara System assets. Returns systems[] (name, path, package). " +
    "path_filter narrows by path prefix (default /Game).",
  input: z.object({
    path_filter: z
      .string()
      .default("/Game")
      .describe("Only return systems whose path starts with this prefix."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const niagaraSystemRead = bridgeTool({
  name: "niagara_system_read",
  domain: "niagara",
  description:
    "Read a Niagara System's structure: emitters (name, enabled, id), " +
    "user_parameters (name, type, value), and metadata (warmup_time).",
  input: z.object({
    system_path: z
      .string()
      .min(1)
      .describe('Full path to the Niagara System asset (e.g. "/Game/Effects/NS_Foam").'),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const niagaraEmitterRead = bridgeTool({
  name: "niagara_emitter_read",
  domain: "niagara",
  description:
    "Read detailed info about one emitter (name, enabled, sim_target) plus its " +
    "scripts and rapid-iteration parameter counts. Select via emitter_name or emitter_index.",
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    ...emitterSelector,
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = { system_path: a.system_path };
    addEmitter(p, a);
    return p;
  },
});

const niagaraModuleGetInputs = bridgeTool({
  name: "niagara_module_get_inputs",
  domain: "niagara",
  description:
    "List rapid-iteration (module override) parameters for an emitter's scripts — " +
    "the editable module inputs overridden from defaults. Returns module_inputs[] " +
    "(script_usage, parameter_name, type, value). Select via emitter_name or emitter_index.",
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    ...emitterSelector,
    script_usage: z
      .string()
      .optional()
      .describe(
        'Optional filter by script usage (e.g. "ParticleUpdate", "EmitterSpawn"). Omit for all scripts.',
      ),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = { system_path: a.system_path };
    addEmitter(p, a);
    if (a.script_usage !== undefined) p.script_usage = a.script_usage;
    return p;
  },
});

// ── User parameters ───────────────────────────────────────────────────────

const niagaraUserParameterAdd = bridgeTool({
  name: "niagara_user_parameter_add",
  domain: "niagara",
  description:
    'Add a user parameter to a Niagara System ("User." prefix added automatically). ' +
    'type_name: float/int32/bool, vector2/vector3/vector4, linear_color, position, ' +
    'material (a User.* UMaterialInterface slot — bind a renderer via ' +
    'niagara_renderer_set_material_binding), or a data interface ("di:Water" / class name).',
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    parameter_name: z
      .string()
      .min(1)
      .describe('Name for the parameter (e.g. "WaterBody"). "User." prefix added automatically.'),
    type_name: z.string().min(1).describe("Type of the parameter (see description)."),
  }),
  annotations: { blockedDuringPie: true },
});

const niagaraUserParameterRemove = bridgeTool({
  name: "niagara_user_parameter_remove",
  domain: "niagara",
  description:
    "Remove a user parameter from a Niagara System. Returns removed_parameter and " +
    "remaining_parameters count.",
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    parameter_name: z
      .string()
      .min(1)
      .describe('Parameter to remove (with or without "User." prefix).'),
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
});

const niagaraUserParameterSet = bridgeTool({
  name: "niagara_user_parameter_set",
  domain: "niagara",
  description:
    "Set a user parameter's default value. float/int/bool: value; vector2: x,y; " +
    "vector3/position: x,y,z; vector4: x,y,z,w; linear_color: r,g,b,a. " +
    "Data interface params cannot be set via MCP — bind at runtime.",
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    parameter_name: z
      .string()
      .min(1)
      .describe('User parameter name (with or without "User." prefix).'),
    ...valueComponents,
  }),
  annotations: { blockedDuringPie: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      system_path: a.system_path,
      parameter_name: a.parameter_name,
    };
    addComponents(p, a);
    return p;
  },
});

// ── Emitter / module tweaks ───────────────────────────────────────────────

const niagaraEmitterSetEnabled = bridgeTool({
  name: "niagara_emitter_set_enabled",
  domain: "niagara",
  description:
    "Enable or disable an emitter within a Niagara System. Select via emitter_name or emitter_index.",
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    enabled: z.boolean().describe("True to enable, False to disable."),
    ...emitterSelector,
  }),
  annotations: { blockedDuringPie: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      system_path: a.system_path,
      enabled: a.enabled,
    };
    addEmitter(p, a);
    return p;
  },
});

const niagaraModuleSetInput = bridgeTool({
  name: "niagara_module_set_input",
  domain: "niagara",
  description:
    "Set a rapid-iteration (module override) parameter on an emitter. Discover names " +
    "with niagara_module_get_inputs first. float/int/bool: value; vector2: x,y; " +
    "vector3/position: x,y,z; vector4: x,y,z,w; linear_color: r,g,b,a. " +
    "Data interface inputs cannot be set via MCP.",
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    parameter_name: z
      .string()
      .min(1)
      .describe("Fully qualified parameter name from niagara_module_get_inputs."),
    ...emitterSelector,
    ...valueComponents,
  }),
  annotations: { blockedDuringPie: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      system_path: a.system_path,
      parameter_name: a.parameter_name,
    };
    addEmitter(p, a);
    addComponents(p, a);
    return p;
  },
});

const niagaraScratchPadModuleAdd = bridgeTool({
  name: "niagara_scratch_pad_module_add",
  domain: "niagara",
  description:
    "Create a scratch pad module with custom HLSL and add it to an emitter's stack. " +
    "Builds a module with typed inputs/outputs, registers it, and inserts it into the " +
    "target_usage stage. Select emitter via emitter_name or emitter_index.",
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    module_name: z
      .string()
      .min(1)
      .describe('Display name for the new module (e.g. "ConformToWater").'),
    target_usage: z
      .string()
      .min(1)
      .describe(
        'Script stage: "ParticleUpdate", "ParticleSpawn", "EmitterUpdate", "EmitterSpawn", ' +
          '"SystemUpdate", "SystemSpawn", "ParticleSimulation", "ParticleEvent".',
      ),
    hlsl: z
      .string()
      .describe(
        "HLSL body. Input/output variable names must match the inputs/outputs array names.",
      ),
    ...emitterSelector,
    inputs: z
      .array(z.object({ name: z.string(), type: z.string() }).passthrough())
      .optional()
      .describe(
        'Input defs, each {"name","type"}. Types: float/int32/bool, vector2/3/4, ' +
          'linear_color, position, or a data interface (e.g. "di:Water").',
      ),
    outputs: z
      .array(z.object({ name: z.string(), type: z.string() }).passthrough())
      .optional()
      .describe('Output defs, each {"name","type"}.'),
    target_index: z
      .number()
      .int()
      .default(-1)
      .describe("Position in the module stack (-1 = end)."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      system_path: a.system_path,
      module_name: a.module_name,
      target_usage: a.target_usage,
      hlsl: a.hlsl,
      target_index: a.target_index,
    };
    addEmitter(p, a);
    if (a.inputs !== undefined) p.inputs = a.inputs;
    if (a.outputs !== undefined) p.outputs = a.outputs;
    return p;
  },
});

// ── Authoring (create / structural) ───────────────────────────────────────

const niagaraSystemCreate = bridgeTool({
  name: "niagara_system_create",
  domain: "niagara",
  description:
    "Create a new, empty Niagara System asset (default System Spawn/Update scripts, no " +
    "emitters — add those with niagara_emitter_add). Fails if an asset already exists " +
    "at system_path (no overwrite).",
  input: z.object({
    system_path: z
      .string()
      .min(1)
      .describe('Full /Game/... destination, e.g. "/Game/FX/NS_Sparks".'),
  }),
  annotations: { blockedDuringPie: true },
});

const niagaraEmitterAdd = bridgeTool({
  name: "niagara_emitter_add",
  domain: "niagara",
  description:
    "Add a blank-but-valid emitter (standard Spawn/Update outputs, no default modules or " +
    "renderer) to a Niagara System. Returns emitter_name (post-uniquify) and emitter_index.",
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    emitter_name: z
      .string()
      .min(1)
      .describe("Name for the new emitter (uniquified if it collides)."),
    sim_target: z
      .string()
      .default("cpu")
      .describe('"cpu" (default) or "gpu".'),
  }),
  annotations: { blockedDuringPie: true },
});

const niagaraEmitterAddRenderer = bridgeTool({
  name: "niagara_emitter_add_renderer",
  domain: "niagara",
  description:
    'Add a renderer to an emitter. renderer_type: "ribbon" (default), "sprite", or "mesh". ' +
    "material_path binds a material (ribbon/sprite only; ignored for mesh). " +
    "Select emitter via emitter_name or emitter_index.",
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    ...emitterSelector,
    renderer_type: z
      .string()
      .default("ribbon")
      .describe('"ribbon" (default), "sprite", or "mesh".'),
    material_path: z
      .string()
      .optional()
      .describe("Optional /Game/... material to bind (ribbon/sprite only)."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      system_path: a.system_path,
      renderer_type: a.renderer_type,
    };
    addEmitter(p, a);
    if (a.material_path !== undefined) p.material_path = a.material_path;
    return p;
  },
});

const niagaraRendererSetMaterial = bridgeTool({
  name: "niagara_renderer_set_material",
  domain: "niagara",
  description:
    "(Re)bind the material on an existing ribbon or sprite renderer. Select emitter via " +
    "emitter_name or emitter_index; renderer_index picks the renderer (default 0).",
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    material_path: z.string().min(1).describe("/Game/... material to bind."),
    ...emitterSelector,
    renderer_index: z
      .number()
      .int()
      .default(0)
      .describe("Which renderer on the emitter (default 0)."),
  }),
  annotations: { blockedDuringPie: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      system_path: a.system_path,
      material_path: a.material_path,
      renderer_index: a.renderer_index,
    };
    addEmitter(p, a);
    return p;
  },
});

const niagaraRendererSetMaterialBinding = bridgeTool({
  name: "niagara_renderer_set_material_binding",
  domain: "niagara",
  description:
    "Bind a ribbon/sprite renderer's material to a User.* Material parameter " +
    "(MaterialUserParamBinding), so a runtime SetVariableMaterial('User.X', MID) drives it. " +
    'Create the Material user param first (niagara_user_parameter_add ... "material"). ' +
    "Select emitter via emitter_name or emitter_index; renderer_index default 0.",
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    user_param_name: z
      .string()
      .min(1)
      .describe(
        'Material user parameter to bind, with or without "User." prefix. Must already exist and be type Material.',
      ),
    ...emitterSelector,
    renderer_index: z
      .number()
      .int()
      .default(0)
      .describe("Which renderer on the emitter (default 0). Ribbon/sprite only."),
  }),
  annotations: { blockedDuringPie: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      system_path: a.system_path,
      user_param_name: a.user_param_name,
      renderer_index: a.renderer_index,
    };
    addEmitter(p, a);
    return p;
  },
});

const niagaraModuleAdd = bridgeTool({
  name: "niagara_module_add",
  domain: "niagara",
  description:
    "Insert a stack module (from an engine UNiagaraScript asset) into a script stage. " +
    "Configure its inputs afterward with niagara_module_set_input. target_usage: " +
    '"ParticleSpawn", "ParticleUpdate", "EmitterSpawn", "EmitterUpdate", "SystemSpawn", "SystemUpdate".',
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    target_usage: z.string().min(1).describe("Which script stage to add to (see description)."),
    module_script_path: z
      .string()
      .min(1)
      .describe(
        'UNiagaraScript module asset, e.g. "/Niagara/Modules/Emitter/SpawnPerUnit.SpawnPerUnit".',
      ),
    ...emitterSelector,
    target_index: z
      .number()
      .int()
      .default(-1)
      .describe("Position in the stack (-1 = append)."),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      system_path: a.system_path,
      target_usage: a.target_usage,
      module_script_path: a.module_script_path,
      target_index: a.target_index,
    };
    addEmitter(p, a);
    return p;
  },
});

const niagaraScriptCreate = bridgeTool({
  name: "niagara_script_create",
  domain: "niagara",
  description:
    "Create a new UNiagaraScript asset shell (empty/seed graph; no graph editing here). " +
    'usage (required): "module", "function", or "dynamic_input". Destination via ' +
    "path/name or asset_path. Auto-saves; no overwrite.",
  input: z.object({
    usage: z
      .string()
      .min(1)
      .describe('Script kind: "module", "function", or "dynamic_input". Required.'),
    path: z.string().default("").describe("Destination folder (with name)."),
    name: z.string().default("").describe("Asset name (with path)."),
    asset_path: z.string().default("").describe("Full destination path (alternative to path/name)."),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = { usage: a.usage };
    if (a.path) p.path = a.path;
    if (a.name) p.name = a.name;
    if (a.asset_path) p.asset_path = a.asset_path;
    return p;
  },
});

const niagaraEmitterSetLocalSpace = bridgeTool({
  name: "niagara_emitter_set_local_space",
  domain: "niagara",
  description:
    "Set an emitter's Local Space flag. Local space = particles simulate in the emitter/owner " +
    "frame (they move + rotate WITH the spawning component) instead of world space. Essential " +
    "for effects authored relative to a MOVING actor — e.g. exhaust/speed streaks that shoot " +
    "backward off a vehicle in its own frame, correct regardless of world heading. Select emitter " +
    "via emitter_name or emitter_index.",
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    local_space: z
      .boolean()
      .describe("true = simulate in the emitter/owner frame; false = world space."),
    ...emitterSelector,
  }),
  annotations: { blockedDuringPie: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      system_path: a.system_path,
      local_space: a.local_space,
    };
    addEmitter(p, a);
    return p;
  },
});

const niagaraRendererSetAlignment = bridgeTool({
  name: "niagara_renderer_set_alignment",
  domain: "niagara",
  description:
    "Set a SPRITE renderer's Alignment and/or FacingMode. alignment=\"velocity\" orients each " +
    "sprite's long axis along its velocity → camera-facing STREAKS (speed lines) instead of round " +
    "billboards; pair with an elongated Sprite Size (long axis = streak length). alignment ∈ " +
    "{unaligned, velocity, custom, automatic}; facing ∈ {camera, camera_plane, custom, " +
    "camera_position, distance_blend, automatic}. Sprite renderers only. Select emitter via " +
    "emitter_name or emitter_index; renderer_index picks the renderer (default 0).",
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    alignment: z
      .enum(["unaligned", "velocity", "custom", "automatic"])
      .optional()
      .describe("Sprite alignment. \"velocity\" = align long axis to velocity (streaks)."),
    facing: z
      .enum(["camera", "camera_plane", "custom", "camera_position", "distance_blend", "automatic"])
      .optional()
      .describe("Sprite facing mode. Default look keeps \"camera\"."),
    ...emitterSelector,
    renderer_index: z
      .number()
      .int()
      .default(0)
      .describe("Which renderer on the emitter (default 0). Sprite only."),
  }),
  annotations: { blockedDuringPie: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      system_path: a.system_path,
      renderer_index: a.renderer_index,
    };
    if (a.alignment !== undefined) p.alignment = a.alignment;
    if (a.facing !== undefined) p.facing = a.facing;
    addEmitter(p, a);
    return p;
  },
});

const niagaraMeshRendererSetMesh = bridgeTool({
  name: "niagara_mesh_renderer_set_mesh",
  domain: "niagara",
  description:
    "Assign a static mesh (and optional uniform scale) to a MESH renderer so it actually renders " +
    "geometry per particle. A mesh renderer added via niagara_emitter_add_renderer starts with NO " +
    "mesh and draws nothing — this is the missing half. The per-particle material comes from the " +
    "assigned mesh's own material slot 0 (bake the look in with mesh_set_static_mesh_material); " +
    "per-particle tint and lifetime reach that material via the standard Particle Color / Particle " +
    "Relative Time nodes. scale defaults to the mesh's authored size — the engine BasicShapes/Cube " +
    "is 100cm, so pass e.g. 0.25 for ~25cm cubes. Select emitter via emitter_name or emitter_index; " +
    "renderer_index picks the renderer (default 0).",
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    mesh_path: z
      .string()
      .min(1)
      .describe("Full /Game/... path to the UStaticMesh to render per particle."),
    scale: z
      .number()
      .positive()
      .optional()
      .describe("Optional uniform mesh scale (engine Cube is 100cm; 0.25 → ~25cm)."),
    ...emitterSelector,
    renderer_index: z
      .number()
      .int()
      .default(0)
      .describe("Which renderer on the emitter (default 0). Must be a mesh renderer."),
  }),
  annotations: { blockedDuringPie: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      system_path: a.system_path,
      mesh_path: a.mesh_path,
      renderer_index: a.renderer_index,
    };
    if (a.scale !== undefined) p.scale = a.scale;
    addEmitter(p, a);
    return p;
  },
});

const niagaraRendererSetEnabled = bridgeTool({
  name: "niagara_renderer_set_enabled",
  domain: "niagara",
  description:
    "Enable or disable a single renderer on an emitter. A disabled renderer keeps its config but " +
    "draws NOTHING — the reliable, cook-safe way to silence a vestigial renderer (e.g. the leftover " +
    "sprite renderer a mesh-particle emitter inherits when duplicated from a sprite system, which " +
    "otherwise co-draws round DefaultSpriteMaterial billboards over the mesh). Zeroing a particle " +
    "attribute like Sprite Size does NOT suppress a renderer whose size isn't bound to it; disabling " +
    "the renderer does. The result always lists ALL renderers (index, type, enabled) so you can " +
    "confirm which index is the sprite vs the mesh without a separate read. Select emitter via " +
    "emitter_name or emitter_index; renderer_index picks the renderer (default 0).",
  input: z.object({
    system_path: z.string().min(1).describe("Full path to the Niagara System asset."),
    enabled: z.boolean().describe("True to show the renderer, false to silence it (draws nothing)."),
    ...emitterSelector,
    renderer_index: z
      .number()
      .int()
      .default(0)
      .describe("Which renderer on the emitter (default 0)."),
  }),
  annotations: { blockedDuringPie: true, idempotentHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      system_path: a.system_path,
      enabled: a.enabled,
      renderer_index: a.renderer_index,
    };
    addEmitter(p, a);
    return p;
  },
});

export const niagaraTools: ToolDef[] = [
  niagaraListSystems,
  niagaraSystemRead,
  niagaraEmitterRead,
  niagaraModuleGetInputs,
  niagaraUserParameterAdd,
  niagaraUserParameterRemove,
  niagaraUserParameterSet,
  niagaraEmitterSetEnabled,
  niagaraModuleSetInput,
  niagaraScratchPadModuleAdd,
  niagaraSystemCreate,
  niagaraEmitterAdd,
  niagaraEmitterAddRenderer,
  niagaraRendererSetMaterial,
  niagaraRendererSetMaterialBinding,
  niagaraModuleAdd,
  niagaraScriptCreate,
  niagaraEmitterSetLocalSpace,
  niagaraRendererSetAlignment,
  niagaraMeshRendererSetMesh,
  niagaraRendererSetEnabled,
];
