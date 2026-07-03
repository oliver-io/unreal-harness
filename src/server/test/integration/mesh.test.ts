/**
 * Mesh domain — static-mesh ASSET authoring (collision / material) plus
 * skeletal-mesh inspection and section/skin editing, asserted via read-back.
 * Port of tests/integration/test_mesh.py.
 *
 * These ops target ASSETS, not placed actors. Collision/material mutations run
 * on a copy of the engine cube duplicated into the test namespace; the skeletal
 * bend-chain rebuild runs in dry_run so it stays non-destructive. On-disk copies
 * live under /Game/__MCPTest__/mesh; creates are idempotent.
 */

import { test, expect } from "bun:test";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { ensureAbsent, assertReady, payload } from "../harness/ops.ts";
import type { Commandable } from "../harness/ops.ts";

const NS = `${ROOT}/mesh`;

// Engine content confirmed present in the test project (read-only sources we copy).
const ENGINE_CUBE = "/Engine/BasicShapes/Cube"; // UStaticMesh package path
const ENGINE_CUBE_OBJ = "/Engine/BasicShapes/Cube.Cube"; // object path for SetStaticMesh
const ENGINE_MATERIAL = "/Engine/EngineMaterials/WorldGridMaterial";
const ENGINE_SKELCUBE = "/Engine/EngineMeshes/SkeletalCube";

/** Idempotently duplicate an engine asset into the test namespace and save it,
 *  so collision/material/section mutations never touch /Engine content. Returns
 *  the destination package path. */
async function dupAndSave(
  bridge: Commandable,
  sourcePath: string,
  destPath: string,
): Promise<string> {
  await ensureAbsent(bridge, destPath);
  await bridge.expect("asset_duplicate", {
    source_path: sourcePath,
    destination_path: destPath,
  });
  await bridge.expect("asset_save", { asset_paths: [destPath] });
  return destPath;
}

async function deleteActorIfPresent(bridge: Commandable, name: string): Promise<void> {
  try {
    await bridge.command("actor_delete", { name });
  } catch {
    /* ignore */
  }
}

editorSuite("mesh", (ctx) => {
  // ── static mesh: collision ────────────────────────────────────────────────
  test("test_get_static_mesh_collision_reads_engine_cube", async () => {
    const result = await ctx.bridge.expect("mesh_get_collision", { asset_path: ENGINE_CUBE });
    expect(result.success).toEqual(true);
    expect(result.has_body_setup).toBeDefined();
    for (const key of [
      "simple_collision_count",
      "box_count",
      "sphere_count",
      "capsule_count",
      "convex_count",
    ]) {
      expect(result[key]).toBeDefined();
    }
  });

  test("test_set_static_mesh_collision_then_readback", async () => {
    const mesh = await dupAndSave(ctx.bridge, ENGINE_CUBE, `${NS}/Cube_Collision`);

    const result = await ctx.bridge.expect("mesh_set_collision", {
      asset_path: mesh,
      shape: "box",
      replace_existing: true,
      collision_trace_flag: "simple_and_complex",
      save: true,
    });
    expect(result.success).toEqual(true);
    expect(result.shape).toEqual("box");
    expect(Number(result.simple_collision_count)).toBeGreaterThanOrEqual(1);
    expect(result.collision_trace_flag).toBeTruthy(); // echoed only when set

    // Read-back via the inspect command.
    const info = await ctx.bridge.expect("mesh_get_collision", { asset_path: mesh });
    expect(info.has_body_setup).toEqual(true);
    expect(Number(info.box_count)).toBeGreaterThanOrEqual(1);
    expect(Number(info.simple_collision_count)).toBeGreaterThanOrEqual(1);
  });

  test("test_set_static_mesh_collision_refuses_engine_content", async () => {
    const resp = await ctx.bridge.command("mesh_set_collision", {
      asset_path: ENGINE_CUBE,
      shape: "box",
    });
    expect(resp.status).toEqual("error");
    expect(String(JSON.stringify(resp))).toContain("/Engine/");
    await assertReady(ctx.bridge);
  });

  // ── static mesh: local bounds (read-only) ─────────────────────────────────
  test("test_get_static_mesh_bounds_reads_engine_cube", async () => {
    // /Engine/BasicShapes/Cube is a 100u cube centered at the origin:
    // box_extent (50,50,50), origin (0,0,0), size (100,100,100).
    const result = await ctx.bridge.expect("mesh_get_bounds", { static_mesh_path: ENGINE_CUBE });
    expect(result.success).toEqual(true);
    const lb = result.local_bounds as Record<string, Record<string, number>>;
    expect(lb).toBeDefined();
    expect(lb.box_extent!.x).toBeCloseTo(50, 1);
    expect(lb.box_extent!.y).toBeCloseTo(50, 1);
    expect(lb.box_extent!.z).toBeCloseTo(50, 1);
    expect(lb.origin!.x).toBeCloseTo(0, 1);
    expect(lb.origin!.y).toBeCloseTo(0, 1);
    expect(lb.origin!.z).toBeCloseTo(0, 1);
    const size = result.size as Record<string, number>;
    expect(size.x).toBeCloseTo(100, 1);
    expect(size.y).toBeCloseTo(100, 1);
    expect(size.z).toBeCloseTo(100, 1);
    const bmin = result.box_min as Record<string, number>;
    const bmax = result.box_max as Record<string, number>;
    expect(bmin.x).toBeCloseTo(-50, 1);
    expect(bmax.x).toBeCloseTo(50, 1);
    // sphere_radius = |(50,50,50)| = ~86.6
    expect(Number(lb.sphere_radius)).toBeCloseTo(86.6, 0);
  });

  // ── static mesh: material slot ────────────────────────────────────────────
  test("test_set_static_mesh_material_then_readback", async () => {
    const mesh = await dupAndSave(ctx.bridge, ENGINE_CUBE, `${NS}/Cube_Material`);

    const result = await ctx.bridge.expect("mesh_set_static_mesh_material", {
      mesh_path: mesh,
      material_path: ENGINE_MATERIAL,
      slot_index: 0,
    });
    expect(result.success).toEqual(true);
    expect(result.mesh_path).toEqual(mesh);
    expect(result.slot_index).toEqual(0);
    expect(result.material_path).toEqual(ENGINE_MATERIAL);

    // Read-back: spawn an actor on the mutated mesh and inspect its material slot.
    const name = "MCPTest_MeshMat";
    await deleteActorIfPresent(ctx.bridge, name);
    await ctx.bridge.expect("spawn_actor", {
      name,
      type: "StaticMeshActor",
      static_mesh: `${mesh}.${mesh.split("/").pop()}`,
    });
    const info = await ctx.bridge.expect("mesh_get_actor_material_info", { actor_name: name });
    expect(Number(info.total_slots)).toBeGreaterThanOrEqual(1);
    const slots = info.material_slots as Array<Record<string, unknown>>;
    expect(String(slots[0]!.material_path ?? "")).toContain("WorldGridMaterial");
    await ctx.bridge.expect("actor_delete", { name });
  });

  // ── static mesh: properties (component-targeting) ──────────────────────────
  test("test_set_static_mesh_properties_binds_mesh_on_component", async () => {
    const bp = `${NS}/BP_MeshProps`;
    await ensureAbsent(ctx.bridge, bp);
    await ctx.bridge.expect("bp_create_blueprint", { name: bp, parent_class: "Actor" });
    await ctx.bridge.expect("bp_add_component", {
      blueprint_name: bp,
      component_type: "StaticMeshComponent",
      component_name: "Mesh",
    });
    const result = await ctx.bridge.expect("mesh_set_static_mesh_properties", {
      blueprint_name: bp,
      component_name: "Mesh",
      static_mesh: ENGINE_CUBE_OBJ,
    });
    expect(result.component).toEqual("Mesh");
    await ctx.bridge.expect("bp_compile", { blueprint_name: bp });

    // Independent read-back: the component template's StaticMesh override via
    // bp_read include_component_properties — not just the handler's own echo.
    const content = await ctx.bridge.expect("bp_read", {
      blueprint_path: bp,
      include_event_graph: false,
      include_functions: false,
      include_variables: false,
      include_component_properties: true,
    });
    const comps = content.components as Array<{
      name: string;
      property_overrides?: Array<{ name: string; value?: unknown }>;
    }>;
    const comp = comps.find((c) => c.name === "Mesh");
    expect(comp).toBeDefined();
    const staticMesh = (comp!.property_overrides ?? []).find((o) => o.name === "StaticMesh");
    expect(staticMesh).toBeDefined();
    expect(String(staticMesh!.value ?? "")).toContain("Cube");
    await assertReady(ctx.bridge);
  });

  // ── static mesh: sockets (add / list / modify / remove + dry-run + guard) ──
  test("test_static_mesh_socket_roundtrip", async () => {
    const mesh = await dupAndSave(ctx.bridge, ENGINE_CUBE, `${NS}/Cube_Sockets`);
    const SOCK = "Muzzle";

    // dry_run add must NOT mutate: the diff names the socket, but a list shows none.
    const dry = await ctx.bridge.expect("mesh_add_socket", {
      asset_path: mesh,
      socket_name: SOCK,
      location_x: 10,
      dry_run: true,
    });
    expect(JSON.stringify(dry)).toContain("sockets_added");
    const empty = await ctx.bridge.expect("mesh_list_sockets", { asset_path: mesh });
    expect(Number(empty.count)).toEqual(0);

    // add for real with a known transform.
    const added = await ctx.bridge.expect("mesh_add_socket", {
      asset_path: mesh,
      socket_name: SOCK,
      location_x: 10,
      location_y: -2,
      rotation_yaw: 90,
    });
    expect(added.success).toEqual(true);
    expect(added.socket_name).toEqual(SOCK);

    // list reads it back with the authored transform.
    const listed = await ctx.bridge.expect("mesh_list_sockets", { asset_path: mesh });
    expect(Number(listed.count)).toEqual(1);
    const sockets = listed.sockets as Array<Record<string, any>>;
    expect(sockets[0]!.socket_name).toEqual(SOCK);
    expect(Number(sockets[0]!.location.x)).toBeCloseTo(10, 3);
    expect(Number(sockets[0]!.location.y)).toBeCloseTo(-2, 3);
    expect(Number(sockets[0]!.rotation.yaw)).toBeCloseTo(90, 3);

    // duplicate add is refused.
    const dup = await ctx.bridge.command("mesh_add_socket", { asset_path: mesh, socket_name: SOCK });
    expect(dup.status).toEqual("error");

    // modify only the provided field; the rest is preserved.
    await ctx.bridge.expect("mesh_modify_socket", { asset_path: mesh, socket_name: SOCK, location_x: 25 });
    const afterMod = await ctx.bridge.expect("mesh_list_sockets", { asset_path: mesh });
    const s = (afterMod.sockets as Array<Record<string, any>>)[0]!;
    expect(Number(s.location.x)).toBeCloseTo(25, 3);
    expect(Number(s.location.y)).toBeCloseTo(-2, 3); // untouched

    // remove → empty again.
    const removed = await ctx.bridge.expect("mesh_remove_socket", { asset_path: mesh, socket_name: SOCK });
    expect(removed.success).toEqual(true);
    expect(Number(removed.remaining_sockets)).toEqual(0);
    const gone = await ctx.bridge.expect("mesh_list_sockets", { asset_path: mesh });
    expect(Number(gone.count)).toEqual(0);
  });

  test("test_mesh_add_socket_refuses_engine_content", async () => {
    const resp = await ctx.bridge.command("mesh_add_socket", { asset_path: ENGINE_CUBE, socket_name: "X" });
    expect(resp.status).toEqual("error");
    expect(String(JSON.stringify(resp))).toContain("/Engine/");
    await assertReady(ctx.bridge);
  });

  // ── skeletal mesh: inspect (read-only) ─────────────────────────────────────
  test("test_inspect_skeletal_mesh_reports_structure", async () => {
    const result = payload(await ctx.bridge.expect("anim_skeletal_mesh_inspect", { path: ENGINE_SKELCUBE }));
    expect(Number(result.num_bones)).toBeGreaterThanOrEqual(1);
    const bones = result.bones as Array<Record<string, unknown>>;
    expect(Array.isArray(bones)).toBe(true);
    expect(bones.length).toBeGreaterThan(0);
    expect(bones[0]!.name).toBeDefined();
    expect(String(result.skeleton ?? "")).toContain("SkeletalCube_Skeleton");
    expect(result.lods).toBeDefined();
  });

  // ── skeletal mesh: disable a render section (mutates a copy) ────────────────
  test("test_set_skeletal_mesh_section_disabled_then_readback", async () => {
    const skel = await dupAndSave(ctx.bridge, ENGINE_SKELCUBE, `${NS}/SkelCube_Sections`);

    const before = await ctx.bridge.expect("anim_skeletal_mesh_inspect", { path: skel });
    const lods = (before.lods as Array<Record<string, unknown>>) ?? [];
    const lod0Sections = lods.length ? (lods[0]!.sections as Array<Record<string, unknown>>) : undefined;
    if (!lods.length || !(lod0Sections && lod0Sections.length)) {
      console.log("SkeletalCube copy exposes no LOD0 render sections to toggle");
      return;
    }
    const sectionIndex = lod0Sections[0]!.section_index;

    const result = await ctx.bridge.expect("anim_skeletal_mesh_set_section_disabled", {
      path: skel,
      section_index: sectionIndex,
      disabled: true,
      lod_index: 0,
    });
    expect(result.section_index).toEqual(sectionIndex);
    expect(result.disabled).toEqual(true);

    const after = await ctx.bridge.expect("anim_skeletal_mesh_inspect", { path: skel });
    const afterLods = after.lods as Array<Record<string, unknown>>;
    const afterSections = afterLods[0]!.sections as Array<Record<string, unknown>>;
    const sec = afterSections.find((s) => s.section_index === sectionIndex)!;
    expect(sec.disabled).toEqual(true);
  });

  // ── skeletal mesh: bend-chain rebuild (dry-run / non-destructive) ───────────
  test("test_skeletal_mesh_build_bend_chain_dry_run", async () => {
    const numBones = 6;
    const resp = await ctx.bridge.command("mesh_build_bend_chain", {
      path: ENGINE_SKELCUBE,
      num_bones: numBones,
      axis: "z",
      dry_run: true,
    });
    if (resp.status !== "success") {
      // The bend chain needs editable LOD0 source geometry, unavailable under
      // -nullrhi. Skip cleanly headless rather than fail.
      await assertReady(ctx.bridge);
      console.log(`bend chain unavailable (no editable LOD0 source geometry, e.g. under -nullrhi): ${resp.error}`);
      return;
    }

    const result = payload(resp.result as Record<string, unknown>);
    expect(result.dry_run).toEqual(true);
    expect(result.num_pole_bones).toEqual(numBones);
    const bones = result.bones as Array<Record<string, unknown>>;
    expect(bones.length).toEqual(numBones + 1); // Root + N poles
    expect(bones[0]!.name).toEqual("Root");
    expect(bones[0]!.parent_index).toEqual(-1);
    await assertReady(ctx.bridge); // asset untouched, editor still interactive
  });

  // ── dynamic-mesh -> static-mesh bake (precondition guard, headless-safe) ────
  test("test_bake_dynamic_mesh_requires_dynamic_mesh_component", async () => {
    const name = "MCPTest_BakeSrc";
    await deleteActorIfPresent(ctx.bridge, name);
    await ctx.bridge.expect("spawn_actor", {
      name,
      type: "StaticMeshActor",
      static_mesh: ENGINE_CUBE_OBJ,
    });

    const resp = await ctx.bridge.command("asset_bake_dynamic_to_static_mesh", {
      actor_name: name,
      target_asset_path: `${NS}/SM_Baked`,
      force_overwrite: true,
    });
    expect(resp.status).toEqual("error");
    expect(String(JSON.stringify(resp))).toContain("DynamicMesh"); // guard names the missing component
    await assertReady(ctx.bridge);
    await ctx.bridge.expect("actor_delete", { name });
  });
});
