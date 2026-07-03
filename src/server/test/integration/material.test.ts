/**
 * Material domain — author Material / MaterialInstance / MaterialFunction / MPC
 * assets, mutate a material's expression graph, wire materials onto actors and
 * Blueprint components, and read the resulting state back. Port of
 * tests/integration/test_material.py.
 *
 * A module-scoped sample Material is created once under the test namespace and
 * reused by the read-only tests; the heavier graph/instance tests build their
 * own assets idempotently.
 */

import { test, expect, beforeAll } from "bun:test";
import { join } from "node:path";
import { existsSync, statSync } from "node:fs";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { ensureAbsent, assertReady , isFalsyOrEmpty } from "../harness/ops.ts";
import { projectDir } from "../harness/env.ts";

const NS = `${ROOT}/material`;
const SAMPLE = `${NS}/M_Sample`;

/** Map a /Game/... content path to its on-disk package file (mirrors
 *  config.uasset_disk_path). The file only exists after the asset is SAVED. */
function uassetDiskPath(gamePath: string, ext = ".uasset"): string {
  const pkg = gamePath.split(".")[0] ?? "";
  if (!pkg.startsWith("/Game/")) throw new Error(`not a /Game/ path: ${gamePath}`);
  const rel = pkg.slice("/Game/".length);
  return join(projectDir(), "Content", rel + ext);
}

function isFile(p: string): boolean {
  return existsSync(p) && statSync(p).isFile();
}

type ExprEntry = {
  name: string;
  inputs: { name: string; connected_expression?: string; connected_output_index?: number }[];
  properties: Record<string, number>;
};

/** Find one expression entry in a material_read graph by node name. */
function exprByName(graph: Record<string, unknown>, name: string): ExprEntry {
  const match = ((graph.expressions as ExprEntry[]) ?? []).find((e) => e.name === name);
  if (!match) throw new Error(`expression ${name} missing from graph: ${JSON.stringify(graph)}`);
  return match;
}

/** add_material_expression returns the new node's name under both the canonical
 *  `expression_name` and the reader-shape alias `name`. */
function exprName(result: Record<string, unknown>): string {
  const name = (result.expression_name as string) || (result.name as string);
  expect(name).toBeTruthy();
  return name;
}

editorSuite("material", (ctx) => {
  // Module-scoped sample material: one bare UMaterial for the whole suite, compiled.
  let sampleMaterial: string;

  beforeAll(async () => {
    await ensureAbsent(ctx.mcp, SAMPLE);
    await ctx.mcp.expect("material_create", { material_path: SAMPLE });
    await ctx.mcp.expect("material_compile", { material_path: SAMPLE });
    await ctx.mcp.expect("asset_save", { asset_paths: [SAMPLE] });
    sampleMaterial = SAMPLE;
  });

  // ── creation: persisted assets land on disk ───────────────────────────────
  test("test_create_material_writes_uasset_on_disk", async () => {
    const path = `${NS}/M_Created`;
    await ensureAbsent(ctx.mcp, path);
    const result = await ctx.mcp.expect("material_create", {
      material_path: path,
      shading_model: "Unlit",
    });
    expect(result.success === false).toBe(false);
    await ctx.mcp.expect("asset_save", { asset_paths: [path] });
    const disk = uassetDiskPath(path);
    expect(isFile(disk)).toBe(true);
  });

  test("test_compile_material", async () => {
    // material_compile recompiles AND persists (GAP-062: it saves the package).
    // Deep observation: a freshly created, never-saved material has no .uasset
    // on disk; after compile the package exists, and the independent reader
    // reports a valid graph. The echoed errors[] alone would be the mutator's
    // own report. Disk path via the LIVE editor's project root (attach-safe).
    const path = `${NS}/M_CompileProbe`;
    await ensureAbsent(ctx.mcp, path);
    try {
      await ctx.mcp.expect("material_create", { material_path: path });
      const pctx = await ctx.mcp.expect("project_context", {});
      const root = (pctx.settings_paths as string[])[0]!;
      const disk = join(root, "Content", path.slice("/Game/".length) + ".uasset");
      // A stale .uasset may survive prior sessions against the live project
      // (ensureAbsent only clears the registry entry) — key on the write
      // timestamp, not bare existence.
      const mtimeBefore = isFile(disk) ? statSync(disk).mtimeMs : null;

      const result = await ctx.mcp.expect("material_compile", { material_path: path });
      // A bare material compiles clean — success not False and no errors reported.
      expect(result.success === false).toBe(false);
      expect(isFalsyOrEmpty(result.errors)).toBe(true);

      // Independent observation 1: compile's save path wrote the package.
      expect(isFile(disk)).toBe(true);
      if (mtimeBefore !== null) {
        expect(statSync(disk).mtimeMs).toBeGreaterThan(mtimeBefore);
      }
      // Independent observation 2: the reader sees a valid compiled graph.
      const graph = await ctx.mcp.expect("material_read", { material_path: path });
      expect(graph.success).toBe(true);
      expect(Array.isArray(graph.expressions)).toBe(true);
    } finally {
      await ensureAbsent(ctx.mcp, path);
    }
  });

  test("test_read_material_graph", async () => {
    const result = await ctx.mcp.expect("material_read", { material_path: sampleMaterial });
    expect(result && typeof result === "object" && Object.keys(result).length > 0).toBeTruthy();
    // The graph reader always reports an expressions collection (possibly empty).
    expect(result.expressions).toBeDefined();
  });

  test("test_set_material_property_flips_flags", async () => {
    // material_set_property flips top-level UMaterial flags after creation;
    // material_read (blend_mode as raw enum value, two_sided as bool) proves
    // the mutation landed. Teardown deletes the asset even on failure — in
    // attach mode it lands in the live project.
    const path = `${NS}/M_SetProp`;
    await ensureAbsent(ctx.mcp, path);
    try {
      await ctx.mcp.expect("material_create", { material_path: path });

      // Factory defaults: Opaque (BLEND_Opaque == 0), single-sided.
      const before = await ctx.mcp.expect("material_read", { material_path: path });
      expect(before.two_sided).toBe(false);
      expect(before.blend_mode).toBe(0);

      const result = await ctx.mcp.expect("material_set_property", {
        material_path: path,
        two_sided: true,
        blend_mode: "Translucent",
      });
      const applied = (result.applied ?? {}) as Record<string, unknown>;
      expect(applied.two_sided).toBe(true);
      expect(applied.blend_mode).toBe("Translucent");

      // Observe through the independent reader: both flags flipped
      // (BLEND_Translucent == 2 in EBlendMode).
      const after = await ctx.mcp.expect("material_read", { material_path: path });
      expect(after.two_sided).toBe(true);
      expect(after.blend_mode).toBe(2);
    } finally {
      await ensureAbsent(ctx.mcp, path);
    }
  });

  // ── full graph authoring round-trip ───────────────────────────────────────
  test("test_material_graph_build_and_teardown", async () => {
    const path = `${NS}/M_Graph`;
    await ensureAbsent(ctx.mcp, path);
    await ctx.mcp.expect("material_create", { material_path: path });

    // Two expressions: an RGB constant and a Multiply node.
    const c3 = exprName(await ctx.mcp.expect("material_add_expression", {
      material_path: path,
      expression_type: "Constant3Vector",
      position_x: -400.0, position_y: 0.0,
      r: 1.0, g: 0.0, b: 0.0,
    }));
    const mul = exprName(await ctx.mcp.expect("material_add_expression", {
      material_path: path,
      expression_type: "Multiply",
      position_x: -150.0, position_y: 0.0,
    }));

    // Wire: Constant3Vector -> Multiply.A, then Multiply -> BaseColor.
    await ctx.mcp.expect("material_connect", {
      material_path: path,
      source_expression: c3,
      target_input: "A",
      target_expression: mul,
      source_output_index: 0,
    });
    await ctx.mcp.expect("material_connect", {
      material_path: path,
      source_expression: mul,
      target_input: "BaseColor",
      target_expression: "Material",
      source_output_index: 0,
    });

    // Mutate the constant's colour. NOTE (verified live): the C++ setter GATES
    // on the `r` key (TryBuildLinearColorFromJson) — a g-only payload is a
    // silent success-shaped NO-OP. The old echo-only assertion hid exactly
    // that; always send r (g/b then zero-fill if omitted).
    await ctx.mcp.expect("material_set_expression_property", {
      material_path: path,
      expression_name: c3,
      r: 0.25, g: 0.5, b: 0.75,
    });

    // Read back through the independent graph reader — structured, not a blob
    // match. Both expressions present; the Multiply's "A" input sources the
    // constant; the material's BaseColor input sources the Multiply; and the
    // constant carries the mutated value.
    const graph = await ctx.mcp.expect("material_read", { material_path: path });
    const c3Node = exprByName(graph, c3);
    const mulNode = exprByName(graph, mul);

    const aInput = mulNode.inputs.find((i) => i.name === "A");
    expect(aInput?.connected_expression).toBe(c3);
    expect(aInput?.connected_output_index).toBe(0);

    const matInputs = graph.material_inputs as Record<string, { connected_expression?: string }>;
    expect(matInputs.BaseColor?.connected_expression).toBe(mul);

    expect(c3Node.properties.r).toBeCloseTo(0.25, 6);
    expect(c3Node.properties.g).toBeCloseTo(0.5, 6);
    expect(c3Node.properties.b).toBeCloseTo(0.75, 6);

    // Delete the Multiply node; it must disappear from the graph.
    await ctx.mcp.expect("material_delete_expression", {
      material_path: path,
      expression_name: mul,
    });
    const graphAfter = await ctx.mcp.expect("material_read", { material_path: path });
    const namesAfter = ((graphAfter.expressions as Record<string, unknown>[]) ?? []).map((e) => e.name);
    expect(namesAfter).not.toContain(mul);
    await assertReady(ctx.mcp);
  });

  // ── material instances ────────────────────────────────────────────────────
  test("test_create_material_instance_writes_uasset_on_disk", async () => {
    const parent = `${NS}/M_InstParent`;
    const inst = `${NS}/MI_Created`;
    await ensureAbsent(ctx.mcp, parent);
    await ensureAbsent(ctx.mcp, inst);
    await ctx.mcp.expect("material_create", { material_path: parent });
    await ctx.mcp.expect("asset_save", { asset_paths: [parent] });

    const result = await ctx.mcp.expect("material_create_instance", {
      asset_path: inst,
      parent_material: parent,
      force_overwrite: true,
    });
    expect(result.success === false).toBe(false);
    await ctx.mcp.expect("asset_save", { asset_paths: [inst] });
    const disk = uassetDiskPath(inst);
    expect(isFile(disk)).toBe(true);
  });

  test("test_material_instance_parameter_round_trip", async () => {
    const parent = `${NS}/M_Param`;
    const inst = `${NS}/MI_Param`;
    await ensureAbsent(ctx.mcp, parent);
    await ensureAbsent(ctx.mcp, inst);

    // Parent material exposes a VectorParameter "Tint" driving BaseColor.
    await ctx.mcp.expect("material_create", { material_path: parent });
    const tint = exprName(await ctx.mcp.expect("material_add_expression", {
      material_path: parent,
      expression_type: "VectorParameter",
      parameter_name: "Tint",
      r: 1.0, g: 0.0, b: 0.0, a: 1.0,
    }));
    await ctx.mcp.expect("material_connect", {
      material_path: parent,
      source_expression: tint,
      target_input: "BaseColor",
      target_expression: "Material",
      source_output_index: 0,
    });
    await ctx.mcp.expect("material_compile", { material_path: parent });
    await ctx.mcp.expect("asset_save", { asset_paths: [parent] });

    // Instance overrides "Tint" green.
    await ctx.mcp.expect("material_create_instance", {
      asset_path: inst,
      parent_material: parent,
      force_overwrite: true,
    });
    await ctx.mcp.expect("material_instance_set_parameter", {
      instance_path: inst,
      parameter_name: "Tint",
      parameter_type: "vector",
      r: 0.0, g: 1.0, b: 0.0, a: 1.0,
    });

    // Read the VALUE back off the instance's own override array — the parameter
    // NAME alone also exists on the parent, so it proves nothing about the set.
    const read = await ctx.mcp.expect("material_read_instance", { instance_path: inst });
    const vec = (read.vector_parameters as { name: string; r: number; g: number; b: number; a: number }[]) ?? [];
    const tintVal = vec.find((p) => p.name === "Tint");
    expect(tintVal).toBeDefined();
    expect(tintVal!.r).toBeCloseTo(0.0, 6);
    expect(tintVal!.g).toBeCloseTo(1.0, 6);
    expect(tintVal!.b).toBeCloseTo(0.0, 6);
    expect(tintVal!.a).toBeCloseTo(1.0, 6);
  });

  test("test_reparent_material_instance", async () => {
    const parentA = `${NS}/M_ParentA`;
    const parentB = `${NS}/M_ParentB`;
    const inst = `${NS}/MI_Reparent`;
    for (const p of [parentA, parentB, inst]) {
      await ensureAbsent(ctx.mcp, p);
    }

    for (const path of [parentA, parentB]) {
      await ctx.mcp.expect("material_create", { material_path: path });
      await ctx.mcp.expect("asset_save", { asset_paths: [path] });
    }

    await ctx.mcp.expect("material_create_instance", {
      asset_path: inst,
      parent_material: parentA,
      force_overwrite: true,
    });
    await ctx.mcp.expect("material_reparent_instance", {
      instance_path: inst,
      new_parent_path: parentB,
    });

    const read = await ctx.mcp.expect("material_read_instance", { instance_path: inst });
    expect(JSON.stringify(read.parent_chain ?? read)).toContain("M_ParentB");
  });

  // ── material function + parameter collection factories ─────────────────────
  test("test_material_function_create_and_read", async () => {
    const path = `${NS}/MF_Sample`;
    await ensureAbsent(ctx.mcp, path);
    const result = await ctx.mcp.expect("material_function_create", {
      asset_path: path,
      description: "MCP test function",
      expose_to_library: false,
    });
    expect(result.success === false).toBe(false);
    // Auto-saves on success -> the package file must exist on disk.
    const disk = uassetDiskPath(path);
    expect(isFile(disk)).toBe(true);

    const read = await ctx.mcp.expect("material_read_function", { function_path: path });
    expect(read && typeof read === "object").toBeTruthy();
    expect(read.expressions).toBeDefined();
  });

  test("test_mpc_create", async () => {
    const path = `${NS}/MPC_Sample`;
    await ensureAbsent(ctx.mcp, path);
    const result = await ctx.mcp.expect("mpc_create", {
      asset_path: path,
      parameters: [
        { type: "scalar", name: "Strength", default_value: 0.5 },
        {
          type: "vector", name: "Tint",
          default_value: { r: 1.0, g: 0.0, b: 0.0, a: 1.0 },
        },
      ],
    });
    expect(result.success === false).toBe(false);
    // Auto-saves on success -> the package file must exist on disk.
    const disk = uassetDiskPath(path);
    expect(isFile(disk)).toBe(true);
  });

  // ── listing + applying materials ──────────────────────────────────────────
  test("test_get_available_materials", async () => {
    const result = await ctx.mcp.expect("material_get_available", {
      search_path: "/Game/",
      include_engine_materials: true,
    });
    expect(result && typeof result === "object" && Object.keys(result).length > 0).toBeTruthy();
  });

  test("test_apply_material_to_actor", async () => {
    // Apply the sample material to a mesh-bearing actor, then read slot 0 back
    // via the independent material-info reader. The actor is spawned at the
    // BRIDGE level with the engine cube (spawn_actor's static_mesh kwarg —
    // house pattern from mesh.test.ts): a mesh-less StaticMeshActor reports
    // zero slots, so the old "info non-empty" assertion never saw the path.
    // Unique per-run name: a fixed name collides with the FName of a
    // previously deleted actor still pending GC (spawn_actor then fails with
    // name-in-use — the known FName-reuse-until-GC wart).
    const actorName = `MCPTest_MatActor_${process.pid}_${Date.now()}`;
    await ctx.bridge.expect("spawn_actor", {
      name: actorName,
      type: "StaticMeshActor",
      static_mesh: "/Engine/BasicShapes/Cube.Cube",
    });
    try {
      const result = await ctx.mcp.expect("material_apply_to_actor", {
        actor_name: actorName,
        material_path: sampleMaterial,
        material_slot: 0,
      });
      expect(result.success === false).toBe(false);

      const info = await ctx.mcp.expect("mesh_get_actor_material_info", { actor_name: actorName });
      expect(Number(info.total_slots)).toBeGreaterThanOrEqual(1);
      const slots = info.material_slots as Array<Record<string, unknown>>;
      expect(slots[0]!.slot).toBe(0);
      // GetPathName() is the full object path (/Game/.../M_Sample.M_Sample).
      expect(String(slots[0]!.material_path ?? "").split(".")[0]).toBe(sampleMaterial);
    } finally {
      await ctx.bridge.command("actor_delete", { name: actorName }).catch(() => undefined);
    }
  });

  test("test_apply_material_to_blueprint", async () => {
    const bp = `${NS}/BP_MatHost`;
    await ensureAbsent(ctx.mcp, bp);
    await ctx.mcp.expect("bp_create_blueprint", { name: bp, parent_class: "Actor" });
    await ctx.mcp.expect("bp_add_component", {
      blueprint_name: bp,
      component_type: "StaticMeshComponent",
      component_name: "Mesh",
      location: [], rotation: [], scale: [],
      component_properties: {},
    });
    // A mesh gives the component a material slot to target.
    await ctx.mcp.expect("mesh_set_static_mesh_properties", {
      blueprint_name: bp,
      component_name: "Mesh",
      static_mesh: "/Engine/BasicShapes/Cube.Cube",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: bp });

    const result = await ctx.mcp.expect("material_apply_to_blueprint", {
      blueprint_name: bp,
      component_name: "Mesh",
      material_path: sampleMaterial,
      material_slot: 0,
    });
    expect(result.success === false).toBe(false);

    // get_blueprint_material_info is a bridge-internal command (no standalone MCP
    // tool), so the read-back goes through the bridge. Assert the applied path
    // landed on slot 0 of the component template — not just "info non-empty".
    const info = await ctx.bridge.expect("get_blueprint_material_info", {
      blueprint_name: bp,
      component_name: "Mesh",
    });
    expect(info.has_static_mesh).toBe(true);
    expect(Number(info.total_slots)).toBeGreaterThanOrEqual(1);
    const slots = info.material_slots as Array<Record<string, unknown>>;
    expect(slots[0]!.slot).toBe(0);
    expect(String(slots[0]!.material_path ?? "").split(".")[0]).toBe(sampleMaterial);
  });

  test("test_set_mesh_material_color", async () => {
    const bp = `${NS}/BP_ColorHost`;
    await ensureAbsent(ctx.mcp, bp);
    await ctx.mcp.expect("bp_create_blueprint", { name: bp, parent_class: "Actor" });
    await ctx.mcp.expect("bp_add_component", {
      blueprint_name: bp,
      component_type: "StaticMeshComponent",
      component_name: "Mesh",
      location: [], rotation: [], scale: [],
      component_properties: {},
    });
    await ctx.mcp.expect("mesh_set_static_mesh_properties", {
      blueprint_name: bp,
      component_name: "Mesh",
      static_mesh: "/Engine/BasicShapes/Cube.Cube",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: bp });

    // GAP-009: this targets a Blueprint component *template* and now REFUSES the
    // dynamic-instance path (a runtime MID in a saved template corrupts level saves).
    // Use call() — the refusal is intentional, carrying error_code feature_disabled.
    const result = await ctx.mcp.call("mesh_set_mesh_material_color", {
      blueprint_name: bp,
      component_name: "Mesh",
      color: [1.0, 0.0, 0.0, 1.0],
      material_path: "/Engine/BasicShapes/BasicShapeMaterial",
      parameter_name: "BaseColor",
      material_slot: 0,
    });
    expect(result.status).toBe("error");
    expect(result.error_code).toBe("feature_disabled");
    expect(JSON.stringify(result)).toContain("material_create_instance");
    await assertReady(ctx.mcp);
  });
});
