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
    const result = await ctx.mcp.expect("material_compile", { material_path: sampleMaterial });
    // A bare material compiles clean — success not False and no errors reported.
    expect(result.success === false).toBe(false);
    expect(isFalsyOrEmpty(result.errors)).toBe(true);
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

    // Mutate a property on the constant (zero-fill semantics: g-only -> (0, 0.5, 0)).
    await ctx.mcp.expect("material_set_expression_property", {
      material_path: path,
      expression_name: c3,
      g: 0.5,
    });

    // Read back: both expressions present.
    const graph = await ctx.mcp.expect("material_read", { material_path: path });
    const blob = JSON.stringify(graph);
    expect(blob).toContain(c3);
    expect(blob).toContain(mul);

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

    const read = await ctx.mcp.expect("material_read_instance", { instance_path: inst });
    expect(JSON.stringify(read.vector_parameters ?? [])).toContain("Tint");
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
    const spawned = await ctx.mcp.expect("actor_spawn", {
      class_path: "/Script/Engine.StaticMeshActor",
      name: "MCPTest_MatActor",
      location: { x: 0.0, y: 0.0, z: 0.0 },
    });
    const actorName = (spawned.actor as Record<string, unknown>).name as string;

    const result = await ctx.mcp.expect("material_apply_to_actor", {
      actor_name: actorName,
      material_path: sampleMaterial,
      material_slot: 0,
    });
    expect(result.success === false).toBe(false);

    const info = await ctx.mcp.expect("mesh_get_actor_material_info", { actor_name: actorName });
    expect(info && typeof info === "object" && Object.keys(info).length > 0).toBeTruthy();
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
    // tool), so the read-back goes through the bridge.
    const info = await ctx.bridge.expect("get_blueprint_material_info", {
      blueprint_name: bp,
      component_name: "Mesh",
    });
    expect(info && typeof info === "object" && Object.keys(info).length > 0).toBeTruthy();
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
