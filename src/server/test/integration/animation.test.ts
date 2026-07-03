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

  /** Return the path of a TRUE UAnimSequence in the project, or null.
   *  Prior test-run artifacts (namespaced dupes, "_MCP*" output-suffix
   *  leftovers) are excluded so discovery is stable across re-runs. */
  async function findAnimSequence(client: Commandable, skeleton = ""): Promise<string | null> {
    const key = skeleton || "*";
    if (discovery.has(key)) return discovery.get(key) ?? null;
    let path: string | null = null;
    try {
      let assets: any[];
      if (skeleton) {
        // Skeleton-compatible discovery (blend-space samples reject sequences
        // from a different skeleton): anim_list_sequences applies the registry
        // Skeleton-tag filter that asset_list lacks.
        const listed = await client.expect("anim_list_sequences", { skeleton_path: skeleton });
        assets = (listed.anim_sequences as any[]) ?? [];
      } else {
        const result = await client.expect("asset_list", {
          directory_path: "/Game",
          recursive: true,
          class_filter: "AnimSequence",
        });
        assets = ((payload(result).assets as any[]) ?? []).filter(
          (a: any) => a.class === "AnimSequence",
        );
      }
      const seqs = assets.filter(
        (a: any) =>
          a.path &&
          String(a.path).startsWith("/Game/") &&
          !String(a.path).includes("_MCP") &&
          !String(a.path).includes("__MCPTest__"),
      );
      path = seqs.length ? (seqs[0].path as string) : null;
    } catch {
      path = null;
    }
    discovery.set(key, path);
    return path;
  }

  /** Attach-safe /Game/... -> Content/....uasset via the LIVE editor's own
   *  project root (B6/D3 precedent — see material.test.ts). */
  async function liveUassetDiskPath(gamePath: string): Promise<string> {
    const pctx = await ctx.mcp.expect("project_context", {});
    const root = (pctx.settings_paths as string[])[0]!;
    const pkg = gamePath.split(".")[0]!;
    return join(root, "Content", pkg.slice("/Game/".length) + ".uasset");
  }

  /** Duplicate a discovered project UAnimSequence into the test namespace so
   *  the mutating sequence ops act on a TEST-OWNED asset. Null when the
   *  project ships no UAnimSequence (callers skip). */
  async function dupeSequence(destName: string): Promise<string | null> {
    const src = await findAnimSequence(ctx.mcp);
    if (!src) return null;
    const dest = `${NS}/${destName}`;
    await ensureAbsent(ctx.mcp, dest);
    await ctx.mcp.expect("asset_duplicate", {
      source_path: src.split(".")[0],
      destination_path: dest,
    });
    return dest;
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
    // Bind a SequencePlayer node's PlayRate (a pin-optional FAnimNode property)
    // to a float variable, and PROVE the bind through an independent reader:
    // binding toggles ShowPinForProperties + ReconstructNode, which materializes
    // the PlayRate pin — absent from bp_list_node_pins before, present after
    // (verified live). The PropertyBindings TMap itself is unobservable (no
    // typed reader; no Python glue for its value struct), so the reconstructed
    // pin is the deepest available observation of the write.
    const name = "ABP_Bind";
    const path = `${NS}/${name}`;
    await ensureAbsent(ctx.mcp, path);
    await ctx.mcp.expect("anim_blueprint_create", {
      name,
      skeleton_path: SKELETON,
      package_path: NS,
    });
    const added = await ctx.mcp.expect("bp_add_node", {
      blueprint_name: path,
      node_type: "SequencePlayer",
      function_name: "AnimGraph",
    });
    const nodeId = added.node_id;
    expect(nodeId).toBeTruthy();
    // A variable for the binding to reference.
    await ctx.mcp.expect("bp_create_variable", {
      blueprint_name: path,
      variable_name: "MCPBindRate",
      variable_type: "float",
    });

    const pinNames = async (): Promise<string[]> => {
      const listed = await ctx.mcp.expect("bp_list_node_pins", {
        blueprint_name: path,
        node_id: nodeId,
        graph_name: "AnimGraph",
      });
      return ((listed.pins as any[]) ?? []).map((p: any) => p.name);
    };

    // Before: the hidden-by-default PlayRate pin is not serialized.
    expect(await pinNames()).not.toContain("PlayRate");

    const result = await ctx.mcp.expect("anim_node_bind_property", {
      blueprint_name: path,
      node_id: nodeId,
      property_name: "PlayRate",
      variable_name: "MCPBindRate",
      graph_name: "AnimGraph",
    });
    expect(result.success).toBe(true);
    expect(result.pin_toggled_visible).toBe(true);

    // Independent readback: the bound pin now exists on the reconstructed node.
    expect(await pinNames()).toContain("PlayRate");
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
  // Each test duplicates a discovered project sequence into the namespace
  // first (asset_duplicate) and acts on the DUPE — real project content is
  // never mutated, and the output-suffix ops' new assets land inside the
  // namespace.

  test("test_list_anim_sequences_enumerates_created_sequence", async () => {
    // Actually CALL anim_list_sequences and prove it against independent
    // state: a freshly-duplicated namespaced sequence must enumerate, count
    // must equal the list length, and every /Game AnimSequence known to the
    // asset registry (asset_list — a different reader) must appear.
    const seq = await dupeSequence("Seq_ListProbe");
    if (!seq) {
      console.log("no anim sequence asset in project");
      return;
    }
    try {
      const listed = await ctx.mcp.expect("anim_list_sequences", {});
      expect(listed.success).toBe(true);
      const entries = (listed.anim_sequences as any[]) ?? [];
      expect(listed.count).toEqual(entries.length);
      const paths = new Set(entries.map((e: any) => String(e.path ?? "").split(".")[0]));
      expect(paths.has(seq)).toBe(true);
      // Cross-check against the independent asset-registry reader.
      const registry = (payload(
        await ctx.mcp.expect("asset_list", {
          directory_path: "/Game",
          recursive: true,
          class_filter: "AnimSequence",
        }),
      ).assets as any[]) ?? [];
      for (const a of registry.filter((a: any) => a.class === "AnimSequence")) {
        expect(paths.has(String(a.path ?? "").split(".")[0])).toBe(true);
      }
      // Documented negative path: a bogus skeleton filter errors loudly.
      const bad = await ctx.mcp.command("anim_list_sequences", {
        skeleton_path: `${NS}/NoSuchSkeleton`,
      });
      expect(bad.status).toEqual("error");
    } finally {
      await ensureAbsent(ctx.mcp, seq);
    }
  });

  test("test_set_anim_sequence_property", async () => {
    const seq = await dupeSequence("Seq_SetProp");
    if (!seq) {
      console.log("no anim sequence asset in project");
      return;
    }
    try {
      const result = await ctx.mcp.expect("anim_sequence_set_property", {
        anim_path: seq,
        additive_anim_type: "LocalSpace",
      });
      // Response echoes the engine-canonical short name (round-trip-safe).
      expect(result.additive_anim_type).toEqual("AAT_LocalSpaceBase");
      expect(result.success).toBe(true);
      // Independent readback via the sanctioned py console hatch: the loaded
      // UAnimSequence's AdditiveAnimType UPROPERTY now reports LocalSpaceBase
      // (no typed reader exposes sequence properties).
      const name = seq.split("/").pop();
      const probe = await ctx.mcp.expect("editor_console_exec", {
        command:
          "py import unreal; " +
          `s = unreal.load_object(None, '${seq}.${name}'); ` +
          "print('MCPTEST_AAT=' + (str(s.get_editor_property('additive_anim_type')) if s else 'NOTFOUND'))",
      });
      expect(String(probe.output ?? "")).toContain("AAT_LOCAL_SPACE_BASE");
    } finally {
      await ensureAbsent(ctx.mcp, seq);
    }
  });

  test("test_extract_anim_between_notifies", async () => {
    const seq = await dupeSequence("Seq_ExtractSrc");
    if (!seq) {
      console.log("no anim sequence asset in project to slice");
      return;
    }
    const out = `${NS}/Anim_Extracted`;
    await ensureAbsent(ctx.mcp, out);
    try {
      const result = await ctx.mcp.expect("anim_extract_between_notifies", {
        source_path: seq,
        dest_name: "Anim_Extracted",
        start_time: 0.0,
        end_time: 0.1,
        dest_path: NS,
      });
      expect(result.success).toBe(true);
      expect(result.name).toEqual("Anim_Extracted");
      // The handler SaveAsset()s the sliced clip — the new .uasset must exist
      // on disk (attach-safe path via the live editor's own project root).
      expect(existsSync(await liveUassetDiskPath(out))).toBe(true);
    } finally {
      await ensureAbsent(ctx.mcp, out);
      await ensureAbsent(ctx.mcp, seq);
    }
  });

  /** Shared deep assertion for the output-suffix ops: act on a namespaced
   *  dupe, assert the echoed output_path is the dupe+suffix INSIDE the
   *  namespace, then confirm the new .uasset on disk (the handlers SaveAsset
   *  the output — verified against MCPIKCommands.cpp). */
  async function outputSuffixRoundtrip(
    dupeName: string,
    op: string,
    suffix: string,
    params: Record<string, unknown>,
  ): Promise<void> {
    const seq = await dupeSequence(dupeName);
    if (!seq) {
      console.log("no anim sequence asset in project");
      return;
    }
    const expectedOut = `${seq}${suffix}`;
    await ensureAbsent(ctx.mcp, expectedOut);
    try {
      const result = await ctx.mcp.expect(op, { anim_path: seq, output_suffix: suffix, ...params });
      expect(result.success).toBe(true);
      expect(String(result.output_path ?? "").split(".")[0]).toEqual(expectedOut);
      expect(existsSync(await liveUassetDiskPath(expectedOut))).toBe(true);
    } finally {
      await ensureAbsent(ctx.mcp, expectedOut);
      await ensureAbsent(ctx.mcp, seq);
    }
  }

  test("test_smooth_anim_sequence", async () => {
    await outputSuffixRoundtrip("Seq_SmoothSrc", "anim_smooth_sequence", "_MCPSmoothed", {
      window_size: 3,
      filter_type: "box",
      smooth_positions: false,
    });
  });

  test("test_normalize_anim_z_offset", async () => {
    await outputSuffixRoundtrip("Seq_ZNormSrc", "anim_normalize_z_offset", "_MCPZNorm", {
      target_z: 0.0,
    });
  });

  test("test_anchor_feet_to_floor", async () => {
    await outputSuffixRoundtrip("Seq_AnchorSrc", "anim_anchor_feet_to_floor", "_MCPFootAnchored", {
      foot_bone_substring: "foot_l",
      pelvis_bone_substring: "pelvis",
      target_z: 0.0,
      sample_frames: 10,
    });
  });
});
