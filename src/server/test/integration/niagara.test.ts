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
import { existsSync, statSync } from "node:fs";
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
// Values are deliberately DISTINCTIVE — InitializeParticle's defaults are
// 0/1/10 scalars and all-ones vectors/colors, so a readback equal to one of
// these can only come from the write, never from an untouched default.
const VALUE_KEYS_FOR_TYPE: Record<string, Record<string, unknown>> = {
  float: { value: 7.25 },
  int32: { value: 5 },
  bool: { value: true },
  vector2: { x: 3.5, y: 4.5 },
  vector3: { x: 1.5, y: 2.5, z: 3.5 },
  vector4: { x: 1.5, y: 2.5, z: 3.5, w: 4.5 },
  linear_color: { r: 0.5, g: 0.25, b: 0.75, a: 0.5 },
  position: { x: 1.5, y: 2.5, z: 3.5 },
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

  /** Attach-safe /Game/... -> Content/....uasset mapping via the live editor's
   *  own project root (project_context.settings_paths[0] = FPaths::ProjectDir()). */
  async function liveUassetDiskPath(gamePath: string): Promise<string> {
    const pctx = await ctx.mcp.expect("project_context", {});
    const root = (pctx.settings_paths as string[])[0]!;
    const pkg = gamePath.split(".")[0]!;
    if (!pkg.startsWith("/Game/")) throw new Error(`not a /Game/ path: ${gamePath}`);
    return join(root, "Content", pkg.slice("/Game/".length) + ".uasset");
  }

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

    // Independent readback: a FRESH niagara_module_get_inputs must report the
    // written value. The setter recompiles and re-bakes the rapid-iteration
    // store (GAP-066), so this proves the write survived the re-bake — not
    // the setter's own echo.
    const after = await ctx.mcp.expect("niagara_module_get_inputs", {
      system_path: sampleSystem,
      emitter_name: name,
    });
    const match = ((after.module_inputs as Record<string, unknown>[]) || []).find(
      (mi) => mi.parameter_name === target.parameter_name,
    );
    expect(match).toBeDefined();
    for (const [key, expected] of Object.entries(VALUE_KEYS_FOR_TYPE[target.type as string]!)) {
      if (typeof expected === "boolean") expect(match![key]).toBe(expected);
      else expect(match![key] as number).toBeCloseTo(expected as number, 4);
    }
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

    // Independent observation via the sanctioned py console hatch: the scratch
    // script must be registered under the emitter's ScratchPads container AND
    // a function-call node referencing it must sit in the target script's
    // graph. No typed niagara reader surfaces scratch modules (verified live
    // 2026-07-03: niagara_emitter_read's script list / RI counts are unchanged
    // by the add, and the custom-HLSL pins are not Module.* rapid-iteration
    // params, so niagara_module_get_inputs shows nothing either).
    const probe = await ctx.mcp.expect("editor_console_exec", {
      command:
        "py import unreal; " +
        "scripts = [o.get_path_name() for o in unreal.ObjectIterator(unreal.NiagaraScript) " +
        `if o.get_name() == 'MCPScratchDouble' and o.get_path_name().startswith('${sampleSystem}.')]; ` +
        "nodes = [n.get_path_name() for n in unreal.ObjectIterator(unreal.NiagaraNodeFunctionCall) " +
        "if n.get_editor_property('function_script') " +
        "and n.get_editor_property('function_script').get_name() == 'MCPScratchDouble' " +
        `and n.get_path_name().startswith('${sampleSystem}.')]; ` +
        "print('MCPTEST_SCRATCH=%d:%d' % (len(scripts), len(nodes)))",
    });
    const m = /MCPTEST_SCRATCH=(\d+):(\d+)/.exec(String(probe.output ?? ""));
    expect(m).not.toBeNull();
    expect(Number(m![1])).toBeGreaterThanOrEqual(1); // scratch script registered
    expect(Number(m![2])).toBeGreaterThanOrEqual(1); // stack node references it
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
    // Value readback through the independent system reader, not the setter's
    // echo (a fresh float user param defaults to 0.0, so 12.5 can only come
    // from the write).
    const afterSet = await ctx.mcp.expect("niagara_system_read", { system_path: sampleSystem });
    const entry = ((afterSet.user_parameters as Record<string, unknown>[]) || []).find(
      (p) => String(p.name).includes(pname),
    );
    expect(entry).toBeDefined();
    expect(entry!.value as number).toBeCloseTo(12.5, 4);

    const rem = await ctx.mcp.expect("niagara_user_parameter_remove", {
      system_path: sampleSystem,
      parameter_name: pname,
    });
    expect(String(rem.removed_parameter)).toContain(pname);
    // Absence readback — the mirror of the add's presence check above.
    const afterRemove = await ctx.mcp.expect("niagara_system_read", { system_path: sampleSystem });
    const stillThere = ((afterRemove.user_parameters as Record<string, unknown>[]) || []).some(
      (p) => String(p.name).includes(pname),
    );
    expect(stillThere).toBe(false);
  });

  // ── standalone script asset ─────────────────────────────────────────────────

  test("test_niagara_script_create", async () => {
    const path = `${NS}/NSC_Sample`;
    await ensureAbsent(ctx.mcp, path);
    // A stale .uasset may survive prior sessions against a live project
    // (ensureAbsent only clears the registry entry when the asset isn't
    // loaded) — key on the write timestamp, not bare existence (B6/D3
    // precedent, see material.test.ts).
    const disk = await liveUassetDiskPath(path);
    const mtimeBefore = existsSync(disk) ? statSync(disk).mtimeMs : null;

    const result = await ctx.mcp.expect("niagara_script_create", {
      usage: "module",
      path: NS,
      name: "NSC_Sample",
    });
    expect(result.success).not.toBe(false);
    expect(String(result.asset_path)).toContain("NSC_Sample");

    // The handler SaveAsset's the new script — the package must be on disk.
    expect(existsSync(disk)).toBe(true);
    if (mtimeBefore !== null) {
      expect(statSync(disk).mtimeMs).toBeGreaterThan(mtimeBefore);
    }
  });
});
