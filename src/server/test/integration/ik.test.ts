/**
 * IK retargeting domain — author a UIKRetargeter, wire rigs/chains, tune ops, and
 * read the state back. Port of tests/integration/test_ik.py.
 *
 * What runs without imported content: `ik_retarget_create` makes a valid (empty)
 * UIKRetargeter and `ik_retarget_read` inspects it, so those two run end-to-end
 * against a stock project. Everything else needs a UIKRigDefinition / AnimSequence
 * / PoseAsset, none of which a stock project ships and none of which an MCP op can
 * create, so those tests discover prerequisites via `asset_list` and skip with a
 * precise reason when absent.
 */

import { test, expect, beforeAll } from "bun:test";
import { existsSync } from "node:fs";
import { join } from "node:path";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { ensureAbsent, assertReady, firstAssetOf, type Commandable } from "../harness/ops.ts";
import { projectDir } from "../harness/env.ts";

const NS = `${ROOT}/ik`;
const RETARGETER = `${NS}/RTG_MCPTest`;

/** Map a /Game/... content path to its on-disk package file. */
function uassetDiskPath(gamePath: string, ext = ".uasset"): string {
  const pkg = gamePath.split(".")[0] ?? gamePath;
  const rel = pkg.slice("/Game/".length);
  return join(projectDir(), "Content", rel + ext);
}

/** First /Game asset of a given class, or null. Used to gate content-dependent
 *  ops (IK rigs, anim sequences, pose assets). */
async function findAsset(bridge: Commandable, classFilter: string): Promise<string | null> {
  const item = await firstAssetOf(
    bridge,
    "asset_list",
    { directory_path: "/Game/", recursive: true, class_filter: classFilter },
    "assets",
  );
  return item ? ((item.path as string) ?? null) : null;
}

editorSuite("ik", (ctx) => {
  // One unconfigured UIKRetargeter for the whole module (no rigs wired).
  let emptyRetargeter = "";
  // A UIKRetargeter wired to an IK rig on both sides (identity retarget), or null
  // when the project ships no IKRigDefinition.
  let configuredRetargeter: { path: string; rig: string } | null = null;

  beforeAll(async () => {
    // empty_retargeter fixture.
    await ensureAbsent(ctx.mcp, RETARGETER);
    await ctx.mcp.expect("ik_retarget_create", { asset_path: RETARGETER });
    emptyRetargeter = RETARGETER;

    // configured_retargeter fixture.
    const rig = await findAsset(ctx.mcp, "IKRigDefinition");
    if (rig) {
      const path = `${NS}/RTG_Configured`;
      await ensureAbsent(ctx.mcp, path);
      await ctx.mcp.expect("ik_retarget_create", {
        asset_path: path,
        source_ik_rig_path: rig,
        target_ik_rig_path: rig,
      });
      configuredRetargeter = { path, rig };
    }
  });

  // ── Always-runnable: create + read ──────────────────────────────────────────

  test("create_ik_retargeter_writes_uasset_on_disk", async () => {
    const path = `${NS}/RTG_Created`;
    await ensureAbsent(ctx.mcp, path);
    const result = await ctx.mcp.expect("ik_retarget_create", { asset_path: path });
    expect(result.success).toBe(true);
    expect(String(result.asset_path ?? "")).toContain("RTG_Created");
    // The handler SaveAsset()s on create, so the package must exist on disk.
    const disk = uassetDiskPath(path);
    expect(existsSync(disk)).toBe(true);
  });

  test("read_ik_retargeter_empty", async () => {
    const result = await ctx.mcp.expect("ik_retarget_read", { retargeter_path: emptyRetargeter });
    expect(String(result.retargeter_path ?? "")).toContain("RTG_MCPTest");
    expect(Array.isArray(result.chain_mappings)).toBe(true);
    // Unconfigured retargeter — neither side is wired to a rig.
    expect(result.source_ik_rig_path ?? "").toEqual("");
    expect(result.target_ik_rig_path ?? "").toEqual("");
  });

  // ── Rig-dependent: skip when the project ships no IK Rig ─────────────────────

  test("set_ik_retargeter_rigs", async () => {
    const rig = await findAsset(ctx.mcp, "IKRigDefinition");
    if (!rig) {
      console.log("SKIP: no IKRigDefinition assets to set on a retargeter");
      return;
    }
    const path = `${NS}/RTG_SetRigs`;
    await ensureAbsent(ctx.mcp, path);
    await ctx.mcp.expect("ik_retarget_create", { asset_path: path });
    await ctx.mcp.expect("ik_retarget_set_rigs", {
      retargeter_path: path,
      source_ik_rig_path: rig,
      target_ik_rig_path: rig,
      rebuild_ops: true,
    });
    const rb = await ctx.mcp.expect("ik_retarget_read", { retargeter_path: path });
    expect(rb.source_ik_rig_path).toBeTruthy();
    expect(rb.target_ik_rig_path).toBeTruthy();
  });

  test("ik_retargeter_auto_map_chains", async () => {
    if (!configuredRetargeter) {
      console.log(
        "SKIP: no IKRigDefinition assets to build a configured retargeter " +
          "(stock project ships none, and there is no create_ik_rig op)",
      );
      return;
    }
    const result = await ctx.mcp.expect("ik_retarget_auto_map_chains", {
      retargeter_path: configuredRetargeter.path,
      match_type: "Fuzzy",
      force_remap: true,
      align_target_pose: true,
    });
    expect(Array.isArray(result.chain_mappings)).toBe(true);
    await assertReady(ctx.mcp);
  });

  test("set_ik_retargeter_chain_mapping", async () => {
    if (!configuredRetargeter) {
      console.log("SKIP: no IKRigDefinition assets to build a configured retargeter");
      return;
    }
    const path = configuredRetargeter.path;
    const rb = await ctx.mcp.expect("ik_retarget_read", { retargeter_path: path });
    const mappings = (rb.chain_mappings as Record<string, unknown>[]) || [];
    if (!mappings.length) {
      console.log("SKIP: configured retargeter exposes no target chains to map");
      return;
    }
    const targetChain = mappings[0]!.target_chain;
    // Identity rig: wire the chain to itself (a name that is guaranteed valid).
    await ctx.mcp.expect("ik_retarget_set_chain_mapping", {
      retargeter_path: path,
      target_chain: targetChain,
      source_chain: targetChain,
    });
    const rb2 = await ctx.mcp.expect("ik_retarget_read", { retargeter_path: path });
    const wired: Record<string, unknown> = {};
    for (const m of (rb2.chain_mappings as Record<string, unknown>[]) ?? []) {
      wired[m.target_chain as string] = m.source_chain;
    }
    expect(wired[targetChain as string]).toEqual(targetChain);
  });

  test("ik_retargeter_align_bones", async () => {
    if (!configuredRetargeter) {
      console.log("SKIP: no IKRigDefinition assets to build a configured retargeter");
      return;
    }
    const result = await ctx.mcp.expect("ik_retarget_align_bones", {
      retargeter_path: configuredRetargeter.path,
      source_or_target: "target",
      reset_first: true,
      excluded_bones: [],
    });
    expect(result.success).not.toBe(false);
    await assertReady(ctx.mcp);
  });

  test("set_ik_retargeter_pelvis_settings", async () => {
    if (!configuredRetargeter) {
      console.log("SKIP: no IKRigDefinition assets to build a configured retargeter");
      return;
    }
    // Needs the Pelvis Motion op, which only exists once the default op stack is
    // built (create with rigs -> AddDefaultOps). Hence the configured fixture.
    const result = await ctx.mcp.expect("ik_retarget_set_pelvis_settings", {
      retargeter_path: configuredRetargeter.path,
      scale_vertical: 0.0,
    });
    expect(result.success).not.toBe(false);
    expect(result.written_fields).toBeDefined();
    expect((result.written_fields as unknown[]) ?? []).toContain("scale_vertical");
  });

  test("set_ik_retargeter_root_motion_settings", async () => {
    if (!configuredRetargeter) {
      console.log("SKIP: no IKRigDefinition assets to build a configured retargeter");
      return;
    }
    const result = await ctx.mcp.expect("ik_retarget_set_root_motion_settings", {
      retargeter_path: configuredRetargeter.path,
      root_height_source: "snap_to_ground",
    });
    expect(result.success).not.toBe(false);
    expect(result.written_fields).toBeDefined();
  });

  // ── Anim/pose-dependent: skip when the project ships no source content ───────

  test("import_ik_retargeter_pose_from_animation", async () => {
    if (!configuredRetargeter) {
      console.log("SKIP: no IKRigDefinition assets to build a configured retargeter");
      return;
    }
    const anim = await findAsset(ctx.mcp, "AnimSequence");
    if (!anim) {
      console.log("SKIP: no UAnimSequence to import a retarget pose from");
      return;
    }
    const result = await ctx.mcp.expect("ik_retarget_import_pose_from_animation", {
      retargeter_path: configuredRetargeter.path,
      anim_sequence_path: anim,
      source_or_target: "source",
      frame_index: 0,
      make_current: true,
    });
    expect(result.success).not.toBe(false);
  });

  test("import_ik_retargeter_pose_from_pose_asset", async () => {
    if (!configuredRetargeter) {
      console.log("SKIP: no IKRigDefinition assets to build a configured retargeter");
      return;
    }
    const pose = await findAsset(ctx.mcp, "PoseAsset");
    if (!pose) {
      console.log("SKIP: no UPoseAsset to import a retarget pose from");
      return;
    }
    const result = await ctx.mcp.expect("ik_retarget_import_pose_from_pose_asset", {
      retargeter_path: configuredRetargeter.path,
      pose_asset_path: pose,
      source_or_target: "source",
      make_current: true,
    });
    expect(result.success).not.toBe(false);
  });

  test("ik_retarget_run_batch", async () => {
    if (!configuredRetargeter) {
      console.log("SKIP: no IKRigDefinition assets to build a configured retargeter");
      return;
    }
    const anim = await findAsset(ctx.mcp, "AnimSequence");
    if (!anim) {
      console.log("SKIP: no source UAnimSequence to duplicate-and-retarget");
      return;
    }
    const result = await ctx.mcp.expect("ik_retarget_run_batch", {
      retargeter_path: configuredRetargeter.path,
      source_animations: [anim],
      name_suffix: "_MCPRetarget",
      include_referenced_assets: true,
      overwrite_existing: false,
    });
    expect(Array.isArray(result.new_assets)).toBe(true);
    await assertReady(ctx.mcp);
  });
});
