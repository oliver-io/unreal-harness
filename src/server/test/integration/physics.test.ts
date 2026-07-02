/**
 * Physics + skeletal-mesh domain. Port of tests/integration/test_physics.py.
 * Inspect a skeletal mesh / physics asset and mutate physics state, reading the
 * result back. Some ops are content-gated (skip when a real UPhysicsAsset / render
 * sections are absent, the common case in a bare fixture / under -nullrhi).
 */

import { test, expect, beforeAll } from "bun:test";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { CommandError } from "../harness/mcpClient.ts";
import { ensureAbsent, assertReady, payload } from "../harness/ops.ts";

const NS = `${ROOT}/physics`;
// Stock engine assets (present out of the box; only mutated in memory, never saved).
const MESH = "/Engine/EngineMeshes/SkeletalCube";

editorSuite("physics", (ctx) => {
  // ── discovery ──────────────────────────────────────────────────────────────
  // Return the path to *some* UPhysicsAsset, or null if the project/engine has
  // none. Tried in order: the asset bound on the engine cube, then a content scan
  // of /Game, then /Engine (class_filter keeps the payload small).
  let physicsAsset: string | null = null;

  beforeAll(async () => {
    const info = await ctx.mcp.expect("anim_skeletal_mesh_inspect", { path: MESH });
    const bound = (info.physics_asset as string) || "";
    if (bound) {
      physicsAsset = bound;
      return;
    }
    for (const directory of ["/Game/", "/Engine/"]) {
      let result: Record<string, unknown>;
      try {
        result = await ctx.mcp.expect("asset_list", {
          directory_path: directory,
          recursive: true,
          class_filter: "PhysicsAsset",
        });
      } catch (e) {
        if (e instanceof CommandError) continue;
        throw e;
      }
      const assets = (result.assets as Record<string, unknown>[]) || [];
      for (const asset of assets) {
        const path = asset.path as string;
        if (path) {
          physicsAsset = path;
          return;
        }
      }
    }
    physicsAsset = null;
  });

  // ── reads (always run) ───────────────────────────────────────────────────────
  test("inspect_skeletal_mesh", async () => {
    const result = payload(await ctx.mcp.expect("anim_skeletal_mesh_inspect", { path: MESH }));
    expect((result.num_bones as number) ?? 0).toBeGreaterThanOrEqual(1);
    expect(Array.isArray(result.bones)).toBe(true);
    expect((result.bones as unknown[]).length).toBeTruthy();
    expect(Array.isArray(result.lods)).toBe(true);
  });

  test("set_physics_properties_on_component", async () => {
    // set_physics_properties operates on a Blueprint component, so build a tiny
    // Actor BP with a primitive component and drive its physics flags.
    const bp = `${NS}/BP_Physics`;
    await ensureAbsent(ctx.mcp, bp);
    await ctx.mcp.expect("bp_create_blueprint", { name: bp, parent_class: "Actor" });
    await ctx.mcp.expect("bp_add_component", {
      blueprint_name: bp,
      component_type: "StaticMeshComponent",
      component_name: "PhysMesh",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: bp });

    const result = await ctx.mcp.expect("physics_set_properties", {
      blueprint_name: bp,
      component_name: "PhysMesh",
      simulate_physics: true,
      gravity_enabled: true,
      mass: 5.0,
      linear_damping: 0.02,
      angular_damping: 0.0,
    });
    expect(result.success).not.toBe(false);
    await assertReady(ctx.mcp);
  });

  test("physics_material_create", async () => {
    // Factory-create a UPhysicalMaterial, verify it landed on disk, and confirm
    // the no-silent-overwrite contract. Mirrors test_physics.py.
    const pm = `${NS}/PM_TestBouncy`;
    await ensureAbsent(ctx.mcp, pm);
    try {
      const result = await ctx.mcp.expect("physics_material_create", {
        asset_path: pm,
        friction: 0.4,
        restitution: 0.95,
        density: 2.0,
        restitution_combine_mode: "Max",
      });
      expect(result.asset_path).toEqual(pm);
      expect(result.restitution as number).toBeCloseTo(0.95, 4);
      expect(result.friction as number).toBeCloseTo(0.4, 4);
      expect(result.density as number).toBeCloseTo(2.0, 4);
      // A passed combine mode flips its per-material override flag; the unset one stays off.
      expect(result.restitution_combine_override).toBe(true);
      expect(result.friction_combine_override).toBe(false);

      const listing = await ctx.mcp.expect("asset_list", {
        directory_path: NS,
        recursive: true,
        class_filter: "PhysicalMaterial",
      });
      // asset_list reports OBJECT paths (/Pkg/Name.Name), not package paths.
      const paths = ((listing.assets as Record<string, unknown>[]) || []).map((a) => a.path as string);
      expect(paths.some((p) => p === pm || p?.startsWith(`${pm}.`))).toBe(true);

      // Uniqueness: a second create at the same path is refused, not overwritten.
      const dup = await ctx.mcp.command("physics_material_create", { asset_path: pm });
      expect(dup.status).not.toEqual("success");
      expect(dup.error_code).toEqual("name_collision");
    } finally {
      await ensureAbsent(ctx.mcp, pm);
    }
    await assertReady(ctx.mcp);
  });

  test("skeletal_mesh_build_bend_chain_dry_run", async () => {
    // dry_run computes the bone-station table WITHOUT mutating/saving the engine
    // mesh. If geometry isn't readable (e.g. render data unavailable headless),
    // the handler errors — document that as a skip rather than a hard failure.
    const resp = await ctx.mcp.command("mesh_build_bend_chain", {
      path: MESH,
      num_bones: 4,
      axis: "z",
      base_fraction: 0.14,
      segment_ratio: 0.85,
      bone_prefix: "pole_",
      root_bone_name: "Root",
      dry_run: true,
    });
    if (resp.status !== "success") {
      // Needs editable LOD0 source geometry (render resource) — null headless.
      await assertReady(ctx.mcp);
      console.log(`SKIP bend-chain preview unavailable (no source geometry, e.g. -nullrhi): ${resp.error}`);
      return;
    }
    const result = payload(resp.result as Record<string, unknown>);
    expect(
      result.dry_run === true || result.bones || result.num_pole_bones !== undefined,
    ).toBeTruthy();
    await assertReady(ctx.mcp);
  });

  test("set_skeletal_mesh_section_disabled", async () => {
    // This op SAVES to disk, so never touch the engine package: duplicate the
    // cube into the test namespace and toggle a section on the copy.
    const copy = `${NS}/SkeletalCubeCopy`;
    await ensureAbsent(ctx.mcp, copy);
    const dup = await ctx.mcp.command("asset_duplicate", {
      source_path: MESH,
      destination_path: copy,
      dry_run: false,
    });
    if (dup.status !== "success") {
      console.log(`SKIP could not duplicate the engine skeletal mesh: ${JSON.stringify(dup)}`);
      return;
    }
    try {
      const info = await ctx.mcp.expect("anim_skeletal_mesh_inspect", { path: copy });
      const lods = (info.lods as Record<string, unknown>[]) || [];
      const sections = lods.length ? (lods[0]!.sections as Record<string, unknown>[]) : null;
      if (!sections || !sections.length) {
        console.log("SKIP no render sections to toggle (render data unavailable, e.g. -nullrhi)");
        return;
      }
      const sectionIndex = (sections[0]!.section_index as number) ?? 0;

      const result = await ctx.mcp.expect("anim_skeletal_mesh_set_section_disabled", {
        path: copy,
        section_index: sectionIndex,
        disabled: true,
        lod_index: 0,
      });
      expect(result.success).not.toBe(false);

      const after = await ctx.mcp.expect("anim_skeletal_mesh_inspect", { path: copy });
      const afterLods = after.lods as Record<string, unknown>[];
      const sec = (afterLods[0]!.sections as Record<string, unknown>[])[sectionIndex]!;
      expect(sec.disabled).toBe(true);
    } finally {
      await ensureAbsent(ctx.mcp, copy);
    }
  });

  // ── physics-asset ops (content-gated) ───────────────────────────────────────
  test("inspect_physics_asset", async () => {
    if (!physicsAsset) {
      console.log("SKIP no PhysicsAsset present in project or engine content");
      return;
    }
    const result = await ctx.mcp.expect("anim_physics_inspect", { path: physicsAsset });
    expect(result.num_bodies).toBeDefined();
    expect(Array.isArray(result.bodies)).toBe(true);
  });

  test("set_physics_body_collision", async () => {
    if (!physicsAsset) {
      console.log("SKIP no PhysicsAsset present in project or engine content");
      return;
    }
    // save=false keeps an engine-owned asset memory-only (no on-disk mutation).
    const result = await ctx.mcp.expect("physics_set_body_collision", {
      path: physicsAsset,
      collision_enabled: "QueryAndPhysics",
      save: false,
    });
    expect(result.num_changed).toBeDefined();
    // Read back: every body should now report the requested collision setting.
    const info = await ctx.mcp.expect("anim_physics_inspect", { path: physicsAsset });
    for (const body of (info.bodies as Record<string, unknown>[]) || []) {
      expect(body.collision_enabled).toEqual("QueryAndPhysics");
    }
    await assertReady(ctx.mcp);
  });

  test("set_physics_constraint_motion", async () => {
    if (!physicsAsset) {
      console.log("SKIP no PhysicsAsset present in project or engine content");
      return;
    }
    const info = await ctx.mcp.expect("anim_physics_inspect", { path: physicsAsset });
    const constraints = (info.constraints as Record<string, unknown>[]) || [];
    if (!constraints.length) {
      console.log("SKIP physics asset has no constraints to edit");
      return;
    }
    const target = constraints[0]!;

    const params: Record<string, unknown> = { path: physicsAsset, save: false, swing1: "Free" };
    if (target.joint_name) {
      params.joint_name = target.joint_name;
    } else {
      // JointName is often None on auto-generated assets — key by bone pair.
      params.bone1 = target.bone1;
      params.bone2 = target.bone2;
    }

    const result = await ctx.mcp.expect("physics_set_constraint_motion", params);
    expect(result.action).toEqual("set_motion");
    expect(result.constraint_index).not.toBeNull();
    expect(result.constraint_index).toBeDefined();
    await assertReady(ctx.mcp);
  });

  test("set_skeletal_mesh_physics_asset", async () => {
    if (!physicsAsset) {
      console.log("SKIP no PhysicsAsset present in project or engine content");
      return;
    }
    // Rebind the engine cube to the discovered asset, then restore — all with
    // save=false so the engine mesh package on disk is never modified.
    const info = await ctx.mcp.expect("anim_skeletal_mesh_inspect", { path: MESH });
    const original = (info.physics_asset as string) || "";
    try {
      const result = await ctx.mcp.expect("mesh_set_physics_asset", {
        path: MESH,
        physics_asset: physicsAsset,
        save: false,
      });
      expect(result.new_physics_asset).toBeTruthy();
    } finally {
      await ctx.mcp.command("mesh_set_physics_asset", {
        path: MESH,
        physics_asset: original,
        save: false,
      });
    }
    await assertReady(ctx.mcp);
  });
});
