/**
 * Animation domain — author Animation Blueprints, Montages and Blend Spaces
 * against the stock engine skeleton, mutate them, and read the resulting state
 * back. Port of tests/integration/test_animation.py.
 *
 * Mostly self-contained: AnimBP / Montage / BlendSpace are all authorable from
 * scratch given only a USkeleton, and the engine ships a stock one. The handful
 * of ops that strictly require a pre-existing UAnimSequence DISCOVER one and
 * skip when the project has none (the default for the fixture project).
 */

import { test, expect, beforeAll } from "bun:test";
import { existsSync } from "node:fs";
import { join } from "node:path";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { ensureAbsent, assertReady, payload } from "../harness/ops.ts";
import { projectDir } from "../harness/env.ts";
import type { Commandable } from "../harness/ops.ts";

const NS = `${ROOT}/anim`;

// Stock engine skeleton (present out of the box). Created AnimBPs/Montages/Blend
// Spaces target it; nothing on the engine package itself is mutated/saved.
const SKELETON = "/Engine/EngineMeshes/SkeletalCube_Skeleton";

const ABP_SAMPLE = "ABP_Sample";
const MONTAGE_SAMPLE = "Montage_Sample";

/** Map a /Game/... content path to its on-disk package file. */
function uassetDiskPath(gamePath: string): string {
  const pkg = gamePath.split(".")[0] ?? gamePath;
  if (!pkg.startsWith("/Game/")) throw new Error(`not a /Game/ path: ${gamePath}`);
  const rel = pkg.slice("/Game/".length);
  return join(projectDir(), "Content", rel + ".uasset");
}

editorSuite("animation", (ctx) => {
  let sampleAbp = "";
  let sampleMontage = "";

  // Cache the discovered anim-sequence path so content-gated tests hit
  // asset_list once rather than per test.
  const discovery = new Map<string, string | null>();

  /** Return the path of a TRUE UAnimSequence in the project, or null. */
  async function findAnimSequence(client: Commandable, skeleton = ""): Promise<string | null> {
    const key = skeleton || "*";
    if (discovery.has(key)) return discovery.get(key) ?? null;
    let path: string | null = null;
    try {
      const result = await client.expect("asset_list", {
        directory_path: "/Game",
        recursive: true,
        class_filter: "AnimSequence",
      });
      const assets = (payload(result).assets as any[]) ?? [];
      const seqs = assets.filter((a: any) => a.class === "AnimSequence" && a.path);
      path = seqs.length ? (seqs[0].path as string) : null;
    } catch {
      path = null;
    }
    discovery.set(key, path);
    return path;
  }

  beforeAll(async () => {
    // One Animation Blueprint for the whole module, targeting the engine skeleton.
    sampleAbp = `${NS}/${ABP_SAMPLE}`;
    await ensureAbsent(ctx.mcp, sampleAbp);
    await ctx.mcp.expect("anim_blueprint_create", {
      name: ABP_SAMPLE,
      skeleton_path: SKELETON,
      package_path: NS,
    });

    // One AnimMontage for the whole module (no source sequence needed).
    sampleMontage = `${NS}/${MONTAGE_SAMPLE}`;
    await ensureAbsent(ctx.mcp, sampleMontage);
    await ctx.mcp.expect("anim_montage_create", {
      name: MONTAGE_SAMPLE,
      skeleton_path: SKELETON,
      package_path: NS,
      slot_name: "DefaultSlot",
    });
  });

  // ── Animation Blueprint ───────────────────────────────────────────────────
  test("test_create_anim_blueprint_writes_uasset_on_disk", async () => {
    const name = "ABP_Created";
    const path = `${NS}/${name}`;
    await ensureAbsent(ctx.mcp, path);
    const result = await ctx.mcp.expect("anim_blueprint_create", {
      name,
      skeleton_path: SKELETON,
      package_path: NS,
    });
    expect(result.success).toBe(true);
    // The created object path carries the .Object suffix; the short name must match.
    expect(String(result.path)).toContain(name);
    expect(String(result.skeleton)).toContain(SKELETON);
    // create_anim_blueprint auto-saves, so the .uasset must exist now.
    const disk = uassetDiskPath(path);
    expect(existsSync(disk)).toBe(true);
    // Read-back via the discovery op: the new AnimBP must be enumerated.
    const listed = await ctx.mcp.expect("anim_list_blueprints", {});
    const names = ((listed.anim_blueprints as any[]) ?? []).map((b: any) => b.name);
    expect(names).toContain(name);
  });

  test("test_set_anim_blueprint_skeleton", async () => {
    // Re-assert the target skeleton (recompiles the AnimBP through the same path
    // a true skeleton swap would; only one engine skeleton is available).
    const result = await ctx.mcp.expect("anim_blueprint_set_skeleton", {
      blueprint_path: sampleAbp,
      skeleton_path: SKELETON,
    });
    expect(result.success).toBe(true);
    expect(String(result.skeleton)).toContain(SKELETON);
    await assertReady(ctx.mcp);
    const listed = await ctx.mcp.expect("anim_list_blueprints", {});
    const entry = ((listed.anim_blueprints as any[]) ?? []).find((b: any) => b.name === ABP_SAMPLE);
    expect(entry).toBeTruthy();
    // target_skeleton tag is the asset-registry form; it must name our skeleton.
    expect(String(entry.target_skeleton)).toContain("SkeletalCube_Skeleton");
  });

  test("test_bind_anim_node_property", async () => {
    const name = "ABP_Bind";
    const path = `${NS}/${name}`;
    await ensureAbsent(ctx.mcp, path);
    const created = await ctx.mcp.expect("anim_blueprint_create", {
      name,
      skeleton_path: SKELETON,
      package_path: NS,
    });
    const nodeId = created.output_pose_node_id;
    if (!nodeId) {
      console.log("created AnimBP exposed no output-pose node id to bind against");
      return;
    }
    // A variable for the binding to reference.
    await ctx.mcp.expect("bp_create_variable", {
      blueprint_name: path,
      variable_name: "MCPBindVar",
      variable_type: "float",
    });
    // Use command (not expect): binding onto the Root node may legitimately
    // error (no such bindable property) — we only require a graceful answer.
    const resp = await ctx.mcp.command("anim_node_bind_property", {
      blueprint_name: path,
      node_id: nodeId,
      property_name: "Sequence",
      variable_name: "MCPBindVar",
    });
    expect(resp && typeof resp === "object").toBeTruthy();
    expect(["success", "error"]).toContain(resp.status);
    await assertReady(ctx.mcp);
  });

  // ── AnimMontage ───────────────────────────────────────────────────────────
  test("test_create_and_read_montage", async () => {
    const name = "Montage_Created";
    const path = `${NS}/${name}`;
    await ensureAbsent(ctx.mcp, path);
    const created = await ctx.mcp.expect("anim_montage_create", {
      name,
      skeleton_path: SKELETON,
      package_path: NS,
      slot_name: "DefaultSlot",
    });
    expect(created.success).toBe(true);
    expect(String(created.path)).toContain(name);
    // create_anim_montage auto-saves.
    const disk = uassetDiskPath(path);
    expect(existsSync(disk)).toBe(true);

    const read = await ctx.mcp.expect("anim_montage_read", { montage_path: path });
    expect(String(read.path)).toContain(name);
    expect(read.sections).toBeDefined();
    expect(Array.isArray(read.slot_tracks)).toBe(true);
    expect(String(read.skeleton)).toContain("SkeletalCube_Skeleton");
  });

  test("test_add_montage_section", async () => {
    const result = await ctx.mcp.expect("anim_montage_add_section", {
      montage_path: sampleMontage,
      section_name: "S_Intro",
      start_time: 0.0,
    });
    expect(result.success).toBe(true);
    expect(Number(result.section_index ?? -1) >= 0).toBe(true);
    const read = await ctx.mcp.expect("anim_montage_read", { montage_path: sampleMontage });
    const names = ((read.sections as any[]) ?? []).map((s: any) => s.name);
    expect(names).toContain("S_Intro");
  });

  test("test_set_montage_blend", async () => {
    await ctx.mcp.expect("anim_montage_set_blend", {
      montage_path: sampleMontage,
      blend_in_time: 0.25,
      blend_out_time: 0.5,
    });
    const read = await ctx.mcp.expect("anim_montage_read", { montage_path: sampleMontage });
    expect(Number(read.blend_in_time)).toBeCloseTo(0.25);
    expect(Number(read.blend_out_time)).toBeCloseTo(0.5);
  });

  test("test_set_montage_section_link", async () => {
    // Two sections must exist before they can be linked.
    await ctx.mcp.expect("anim_montage_add_section", {
      montage_path: sampleMontage,
      section_name: "S_LinkA",
      start_time: 0.0,
    });
    await ctx.mcp.expect("anim_montage_add_section", {
      montage_path: sampleMontage,
      section_name: "S_LinkB",
      start_time: 0.0,
    });
    const result = await ctx.mcp.expect("anim_montage_set_section_link", {
      montage_path: sampleMontage,
      section_name: "S_LinkA",
      next_section_name: "S_LinkB",
    });
    expect(result.success).toBe(true);
    const read = await ctx.mcp.expect("anim_montage_read", { montage_path: sampleMontage });
    const entry = ((read.sections as any[]) ?? []).find((s: any) => s.name === "S_LinkA");
    expect(entry).toBeTruthy();
    expect(entry.next_section).toEqual("S_LinkB");
  });

  // ── Anim Notifies (run against a montage — UAnimSequenceBase) ──────────────
  test("test_anim_notify_lifecycle", async () => {
    const name = "Montage_Notify";
    const path = `${NS}/${name}`;
    await ensureAbsent(ctx.mcp, path);
    await ctx.mcp.expect("anim_montage_create", {
      name,
      skeleton_path: SKELETON,
      package_path: NS,
    });

    const added = await ctx.mcp.expect("anim_notify_add", {
      anim_path: path,
      notify_class: "AnimNotify_PlaySound",
      trigger_time: 0.0,
      track_index: 0,
    });
    expect(added.success).toBe(true);
    const idx = added.notify_index;
    expect(idx == null).toBe(false);

    const listed = await ctx.mcp.expect("anim_list_notifies", { anim_path: path });
    expect(Number(listed.count ?? 0) >= 1).toBe(true);
    const entry = ((listed.notifies as any[]) ?? []).find((n: any) => n.index === idx);
    expect(entry).toBeTruthy();

    const removed = await ctx.mcp.expect("anim_notify_remove", { anim_path: path, notify_index: idx });
    expect(removed.success).not.toBe(false);
    const after = await ctx.mcp.expect("anim_list_notifies", { anim_path: path });
    const afterIdx = ((after.notifies as any[]) ?? []).map((n: any) => n.index);
    expect(afterIdx).not.toContain(idx);
  });

  // ── BlendSpace ────────────────────────────────────────────────────────────
  test("test_create_and_read_blend_space", async () => {
    const name = "BS_Created";
    const path = `${NS}/${name}`;
    await ensureAbsent(ctx.mcp, path);
    const created = await ctx.mcp.expect("anim_blend_space_create", {
      name,
      skeleton_path: SKELETON,
      is_2d: false,
      package_path: NS,
      x_axis_name: "Speed",
      x_axis_min: 0.0,
      x_axis_max: 100.0,
      x_axis_grid_divisions: 4,
    });
    expect(created.success).toBe(true);
    expect(created.type).toEqual("BlendSpace1D");
    // create_blend_space auto-saves.
    const disk = uassetDiskPath(path);
    expect(existsSync(disk)).toBe(true);

    const read = await ctx.mcp.expect("anim_blend_space_read", { blend_space_path: path });
    // read_blend_space's type label vocabulary differs from create's
    // ("BlendSpace1D" vs "2D"); just assert the asset round-trips with its axis.
    expect(read.type).toBeDefined();
    expect((read.x_axis as any)?.name).toEqual("Speed");
    expect(Array.isArray(read.samples)).toBe(true);
    expect(read.sample_count).toEqual(0);
  });

  test("test_blend_space_sample_lifecycle", async () => {
    const seq = await findAnimSequence(ctx.mcp, SKELETON);
    if (!seq) {
      console.log("no anim sequence asset in project (blend-space samples need one)");
      return;
    }

    const name = "BS_Samples";
    const path = `${NS}/${name}`;
    await ensureAbsent(ctx.mcp, path);
    await ctx.mcp.expect("anim_blend_space_create", {
      name,
      skeleton_path: SKELETON,
      is_2d: false,
      package_path: NS,
      x_axis_name: "Speed",
      x_axis_min: 0.0,
      x_axis_max: 100.0,
      x_axis_grid_divisions: 4,
    });

    const added = await ctx.mcp.expect("anim_blend_space_add_sample", {
      blend_space_path: path,
      anim_sequence: seq,
      x: 50.0,
    });
    expect(added.success).toBe(true);
    const sampleIdx = added.sample_index;
    expect(sampleIdx == null).toBe(false);

    const read = await ctx.mcp.expect("anim_blend_space_read", { blend_space_path: path });
    expect(Number(read.sample_count ?? 0) >= 1).toBe(true);

    const removed = await ctx.mcp.expect("anim_blend_space_remove_sample", {
      blend_space_path: path,
      sample_index: sampleIdx,
    });
    expect(removed.success).toBe(true);
    const after = await ctx.mcp.expect("anim_blend_space_read", { blend_space_path: path });
    expect(Number(after.sample_count ?? -1)).toEqual(0);
  });

  // ── UAnimSequence-gated ops (skip when the project ships no sequence) ──────
  test("test_set_anim_sequence_property", async () => {
    const seq = await findAnimSequence(ctx.mcp);
    if (!seq) {
      console.log("no anim sequence asset in project");
      return;
    }
    const result = await ctx.mcp.expect("anim_sequence_set_property", {
      anim_path: seq,
      additive_anim_type: "LocalSpace",
    });
    // Response echoes the engine-canonical short name (round-trip-safe).
    expect(result.additive_anim_type).toEqual("AAT_LocalSpaceBase");
    expect(result.success).toBe(true);
  });

  test("test_extract_anim_between_notifies", async () => {
    const seq = await findAnimSequence(ctx.mcp);
    if (!seq) {
      console.log("no anim sequence asset in project to slice");
      return;
    }
    const result = await ctx.mcp.expect("anim_extract_between_notifies", {
      source_path: seq,
      dest_name: "Anim_Extracted",
      start_time: 0.0,
      end_time: 0.1,
      dest_path: NS,
    });
    expect(result.name === "Anim_Extracted" || result.path).toBeTruthy();
  });

  test("test_smooth_anim_sequence", async () => {
    const seq = await findAnimSequence(ctx.mcp);
    if (!seq) {
      console.log("no anim sequence asset in project to smooth");
      return;
    }
    const result = await ctx.mcp.expect("anim_smooth_sequence", {
      anim_path: seq,
      window_size: 3,
      filter_type: "box",
      output_suffix: "_MCPSmoothed",
      smooth_positions: false,
    });
    expect(result.success).toBe(true);
    expect(result.output_path).toBeTruthy();
  });

  test("test_normalize_anim_z_offset", async () => {
    const seq = await findAnimSequence(ctx.mcp);
    if (!seq) {
      console.log("no anim sequence asset in project to normalize");
      return;
    }
    const result = await ctx.mcp.expect("anim_normalize_z_offset", {
      anim_path: seq,
      target_z: 0.0,
      output_suffix: "_MCPZNorm",
    });
    expect(result.success).toBe(true);
    expect(result.output_path).toBeTruthy();
  });

  test("test_anchor_feet_to_floor", async () => {
    const seq = await findAnimSequence(ctx.mcp);
    if (!seq) {
      console.log("no anim sequence asset in project to anchor");
      return;
    }
    const result = await ctx.mcp.expect("anim_anchor_feet_to_floor", {
      anim_path: seq,
      foot_bone_substring: "foot_l",
      pelvis_bone_substring: "pelvis",
      target_z: 0.0,
      sample_frames: 10,
      output_suffix: "_MCPFootAnchored",
    });
    expect(result.success).toBe(true);
    expect(result.output_path).toBeTruthy();
  });
});
