/**
 * Niagara domain — build a system from scratch, then mutate and read it back.
 * Port of tests/integration/test_niagara.py.
 *
 * A module-scoped sample Niagara System (plus one baseline CPU emitter) and a
 * trivial material are created once in beforeAll and reused. Each test arranges
 * any extra prerequisite state (its own emitter, a renderer, a user parameter),
 * dispatches the op under test (`mcp.expect` throws on a non-success envelope),
 * then asserts the resulting state via a read/inspect op.
 *
 * Bridge command names and param keys mirror the C++ handlers exactly — NOT the
 * Python tool kwarg names. Structural ops echo the post-uniquify emitter name
 * back under `emitter_name`; read_niagara_system reports emitters under
 * `emitters` (each: name/id/enabled).
 */

import { test, expect, beforeAll } from "bun:test";
import { existsSync } from "node:fs";
import { join } from "node:path";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { ensureAbsent } from "../harness/ops.ts";
import { projectDir } from "../harness/env.ts";

const NS = `${ROOT}/niagara`;
const SAMPLE = `${NS}/NS_Sample`;
const MATERIAL = `${NS}/M_Sample`;

// A stock engine module guaranteed to exist wherever the Niagara plugin is
// enabled; adding it to ParticleSpawn yields rapid-iteration inputs to poke.
const INIT_PARTICLE_MODULE =
  "/Niagara/Modules/Spawn/Initialization/InitializeParticle.InitializeParticle";

// Value keys (besides the system/emitter/parameter identifiers) that the value
// setters accept per Niagara type, used to build a valid set_* payload.
const VALUE_KEYS_FOR_TYPE: Record<string, Record<string, unknown>> = {
  float: { value: 1.0 },
  int32: { value: 2 },
  bool: { value: true },
  vector2: { x: 1.0, y: 2.0 },
  vector3: { x: 1.0, y: 2.0, z: 3.0 },
  vector4: { x: 1.0, y: 2.0, z: 3.0, w: 4.0 },
  linear_color: { r: 0.5, g: 0.25, b: 0.75, a: 1.0 },
  position: { x: 1.0, y: 2.0, z: 3.0 },
};

/** Map a /Game/... content path to its on-disk package file (the file only
 *  exists after the asset is SAVED). Mirrors config.uasset_disk_path. */
function uassetDiskPath(gamePath: string, ext = ".uasset"): string {
  const pkg = gamePath.split(".")[0] ?? gamePath;
  const rel = pkg.slice("/Game/".length);
  return join(projectDir(), "Content", rel + ext);
}

interface Commandable {
  expect(type: string, params?: Record<string, unknown>): Promise<Record<string, unknown>>;
}

/** Add a fresh emitter and return its resolved (post-uniquify) name. Lets each
 *  mutation test own its emitter, so tests stay order-independent. */
async function addEmitter(
  bridge: Commandable,
  systemPath: string,
  name: string,
): Promise<string> {
  const res = await bridge.expect("niagara_emitter_add", {
    system_path: systemPath,
    emitter_name: name,
    sim_target: "cpu",
  });
  return (res.emitter_name as string) || name;
}

editorSuite("niagara", (ctx) => {
  let sampleSystem = "";
  let sampleEmitter = "";
  let sampleMaterial = "";

  beforeAll(async () => {
    // sample_system: one empty Niagara System plus a single baseline CPU emitter.
    await ensureAbsent(ctx.mcp, SAMPLE);
    await ctx.mcp.expect("niagara_system_create", { system_path: SAMPLE });
    const added = await ctx.mcp.expect("niagara_emitter_add", {
      system_path: SAMPLE,
      emitter_name: "Sample01",
      sim_target: "cpu",
    });
    sampleSystem = SAMPLE;
    sampleEmitter = (added.emitter_name as string) || "Sample01";

    // sample_material: a trivial material to bind onto sprite/ribbon renderers.
    await ensureAbsent(ctx.mcp, MATERIAL);
    await ctx.mcp.expect("material_create", { material_path: MATERIAL });
    sampleMaterial = MATERIAL;
  });

  // ── create / list / read ───────────────────────────────────────────────────

  test("test_create_system_writes_uasset_on_disk", async () => {
    const path = `${NS}/NS_Created`;
    await ensureAbsent(ctx.mcp, path);
    const result = await ctx.mcp.expect("niagara_system_create", { system_path: path });
    expect(result.success).toBe(true);
    expect(result.emitter_count).toEqual(0);
    // The create handler SaveAsset's the package, so the file must be on disk.
    const disk = uassetDiskPath(path);
    expect(existsSync(disk)).toBe(true);
  });

  test("test_read_niagara_system", async () => {
    const result = await ctx.mcp.expect("niagara_system_read", { system_path: sampleSystem });
    const names = ((result.emitters as Record<string, unknown>[]) || []).map((e) => e.name);
    expect(names).toContain(sampleEmitter);
  });

  test("test_list_niagara_systems", async () => {
    const result = await ctx.mcp.expect("niagara_list_systems", { path_filter: NS });
    const paths = ((result.systems as Record<string, unknown>[]) || []).map((s) => s.path);
    const blob = String(paths);
    expect(blob).toContain("NS_Sample");
  });

  // ── emitters ────────────────────────────────────────────────────────────────

  test("test_emitter_add_then_read", async () => {
    const name = await addEmitter(ctx.mcp, sampleSystem, "Extra01");
    const result = await ctx.mcp.expect("niagara_emitter_read", {
      system_path: sampleSystem,
      emitter_name: name,
    });
    expect(result.name).toEqual(name);
  });

  test("test_set_emitter_enabled", async () => {
    const name = await addEmitter(ctx.mcp, sampleSystem, "Toggle01");
    await ctx.mcp.expect("niagara_emitter_set_enabled", {
      system_path: sampleSystem,
      emitter_name: name,
      enabled: false,
    });
    // Confirm the persisted state via a read of the whole system.
    const sysinfo = await ctx.mcp.expect("niagara_system_read", { system_path: sampleSystem });
    const match = ((sysinfo.emitters as Record<string, unknown>[]) || []).find(
      (e) => e.name === name,
    );
    expect(match).toBeDefined();
    expect(match!.enabled).toBe(false);
  });

  // ── renderers ─────────────────────────────────────────────────────────────

  test("test_emitter_add_renderer", async () => {
    const name = await addEmitter(ctx.mcp, sampleSystem, "Rend01");
    const result = await ctx.mcp.expect("niagara_emitter_add_renderer", {
      system_path: sampleSystem,
      emitter_name: name,
      renderer_type: "sprite",
      material_path: sampleMaterial,
    });
    expect(result.renderer_type).toEqual("sprite");
    expect(result.renderer_index).toEqual(0);
  });

  test("test_renderer_set_material", async () => {
    const name = await addEmitter(ctx.mcp, sampleSystem, "RendMat01");
    await ctx.mcp.expect("niagara_emitter_add_renderer", {
      system_path: sampleSystem,
      emitter_name: name,
      renderer_type: "sprite",
    });
    const result = await ctx.mcp.expect("niagara_renderer_set_material", {
      system_path: sampleSystem,
      emitter_name: name,
      renderer_index: 0,
      material_path: sampleMaterial,
    });
    expect(String(result.material)).toContain("M_Sample");
  });

  test("test_renderer_set_material_binding", async () => {
    const name = await addEmitter(ctx.mcp, sampleSystem, "RendBind01");
    // The renderer's MaterialUserParamBinding needs a Material-typed User.* slot.
    await ctx.mcp.expect("niagara_user_parameter_add", {
      system_path: sampleSystem,
      parameter_name: "RibbonMaterial",
      type_name: "material",
    });
    await ctx.mcp.expect("niagara_emitter_add_renderer", {
      system_path: sampleSystem,
      emitter_name: name,
      renderer_type: "sprite",
    });
    const result = await ctx.mcp.expect("niagara_renderer_set_material_binding", {
      system_path: sampleSystem,
      emitter_name: name,
      renderer_index: 0,
      user_param_name: "RibbonMaterial",
    });
    expect(String(result.user_param_name)).toContain("RibbonMaterial");
  });

  // ── modules ─────────────────────────────────────────────────────────────────

  test("test_module_add_inputs_and_set", async () => {
    const name = await addEmitter(ctx.mcp, sampleSystem, "Mod01");

    const add = await ctx.mcp.expect("niagara_module_add", {
      system_path: sampleSystem,
      emitter_name: name,
      target_usage: "ParticleSpawn",
      module_script_path: INIT_PARTICLE_MODULE,
    });
    expect(add.module_node_created).toBe(true);

    const inputs = await ctx.mcp.expect("niagara_module_get_inputs", {
      system_path: sampleSystem,
      emitter_name: name,
    });
    const moduleInputs = (inputs.module_inputs as Record<string, unknown>[]) || [];
    if (!moduleInputs.length) {
      console.log("skip: module added but no rapid-iteration inputs were exposed to set");
      return;
    }

    // Pick the first input whose type we know how to write a value for.
    const target = moduleInputs.find(
      (mi) => (mi.type as string) in VALUE_KEYS_FOR_TYPE,
    );
    if (!target) {
      console.log(
        `skip: no settable scalar/vector module input among: ${moduleInputs.map((mi) => mi.type)}`,
      );
      return;
    }

    const payload: Record<string, unknown> = {
      system_path: sampleSystem,
      emitter_name: name,
      parameter_name: target.parameter_name,
      ...VALUE_KEYS_FOR_TYPE[target.type as string],
    };
    const result = await ctx.mcp.expect("niagara_module_set_input", payload);
    expect(result.parameter_name).toEqual(target.parameter_name);
  });

  test("test_add_scratch_pad_module", async () => {
    const name = await addEmitter(ctx.mcp, sampleSystem, "Scratch01");
    const result = await ctx.mcp.expect("niagara_scratch_pad_module_add", {
      system_path: sampleSystem,
      emitter_name: name,
      module_name: "MCPScratchDouble",
      target_usage: "ParticleUpdate",
      hlsl: "OutValue = InValue * 2.0;",
      inputs: [{ name: "InValue", type: "float" }],
      outputs: [{ name: "OutValue", type: "float" }],
    });
    expect(result.success).toBe(true);
    expect(result.module_node_created).toBe(true);
  });

  // ── user parameters ─────────────────────────────────────────────────────────

  test("test_user_parameter_lifecycle", async () => {
    const pname = "MCPSpeed";

    await ctx.mcp.expect("niagara_user_parameter_add", {
      system_path: sampleSystem,
      parameter_name: pname,
      type_name: "float",
    });
    // It should now appear (with the User. prefix) on a read.
    const sysinfo = await ctx.mcp.expect("niagara_system_read", { system_path: sampleSystem });
    const userParams = (sysinfo.user_parameters as Record<string, unknown>[]) || [];
    expect(userParams.some((p) => String(p.name).includes(pname))).toBeTruthy();

    const setres = await ctx.mcp.expect("niagara_user_parameter_set", {
      system_path: sampleSystem,
      parameter_name: pname,
      value: 12.5,
    });
    expect(setres.success).toBe(true);

    const rem = await ctx.mcp.expect("niagara_user_parameter_remove", {
      system_path: sampleSystem,
      parameter_name: pname,
    });
    expect(String(rem.removed_parameter)).toContain(pname);
  });

  // ── standalone script asset ─────────────────────────────────────────────────

  test("test_niagara_script_create", async () => {
    const path = `${NS}/NSC_Sample`;
    await ensureAbsent(ctx.mcp, path);
    const result = await ctx.mcp.expect("niagara_script_create", {
      usage: "module",
      path: NS,
      name: "NSC_Sample",
    });
    expect(result.success).not.toBe(false);
    expect(String(result.asset_path)).toContain("NSC_Sample");
  });
});
