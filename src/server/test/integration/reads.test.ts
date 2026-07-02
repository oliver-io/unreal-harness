/**
 * Read-only / project / editor-utility ops. Port of tests/integration/test_reads.py.
 * Most work against an empty project; a few are content-gated (create a BP, point
 * at an engine skeleton, or accept a structured error envelope for absent assets).
 */

import { test, expect, beforeAll } from "bun:test";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { ensureAbsent, assertReady, firstAssetOf } from "../harness/ops.ts";

const NS = `${ROOT}/reads`;
const SAMPLE_BP = `${NS}/BP_R`;
// A skeleton that ships with the engine — present in every install.
const ENGINE_SKELETON = "/Engine/EngineMeshes/SkeletalCube_Skeleton";

editorSuite("reads", (ctx) => {
  beforeAll(async () => {
    // One Actor Blueprint for the bp_read test (create/compile covered elsewhere).
    await ensureAbsent(ctx.mcp, SAMPLE_BP);
    await ctx.mcp.expect("bp_create_blueprint", { name: SAMPLE_BP, parent_class: "Actor" });
    await ctx.mcp.expect("bp_compile", { blueprint_name: SAMPLE_BP });
  });

  // ── pure reads / project ──────────────────────────────────────────────────
  test("scene_brief", async () => {
    const r = await ctx.mcp.expect("scene_brief", {});
    expect(r.world_name).toBeDefined();
    expect(r.by_class).toBeDefined();
    expect(Array.isArray(r.skipped_sublevels)).toBe(true);
  });

  test("project_context", async () => {
    const r = await ctx.mcp.expect("project_context", {});
    expect(r.name).toBeTruthy();
    expect(r.engine_version).toBeTruthy();
    expect(Array.isArray(r.plugins)).toBe(true);
    expect(Array.isArray(r.modules)).toBe(true);
  });

  test("editor_perf_snapshot", async () => {
    const r = await ctx.mcp.expect("editor_perf_snapshot", {});
    expect(r.memory && typeof r.memory === "object").toBeTruthy();
  });

  test("bp_read", async () => {
    const r = await ctx.mcp.expect("bp_read", { blueprint_path: SAMPLE_BP });
    expect(r && typeof r === "object").toBeTruthy();
  });

  // ── list_* asset reads (empty-project-safe) ───────────────────────────────
  const listCases: [string, Record<string, unknown>, string][] = [
    ["anim_list_skeletons", {}, "skeletons"],
    ["anim_list_blueprints", {}, "anim_blueprints"],
    ["anim_list_montages", {}, "montages"],
    ["anim_list_sequences", {}, "anim_sequences"],
    ["anim_list_blend_spaces", {}, "blend_spaces"],
    ["anim_list_layer_interfaces", {}, "layer_interfaces"],
    ["anim_skeleton_list_sockets", { skeleton_path: ENGINE_SKELETON }, "sockets"],
  ];
  for (const [tool, args, key] of listCases) {
    test(tool, async () => {
      const r = await ctx.mcp.expect(tool, args);
      expect(Array.isArray(r[key])).toBe(true);
    });
  }

  test("anim_list_notifies", async () => {
    const anim = await firstAssetOf(ctx.mcp, "anim_list_sequences", {}, "anim_sequences");
    if (anim && anim.path) {
      const r = await ctx.mcp.expect("anim_list_notifies", { anim_path: anim.path });
      expect(Array.isArray(r.notifies)).toBe(true);
    } else {
      const resp = await ctx.mcp.command("anim_list_notifies", { anim_path: `${NS}/NoSuchAnim` });
      expect(["success", "error"]).toContain(resp.status);
    }
  });

  test("ik_rig_list_chains", async () => {
    const resp = await ctx.mcp.command("ik_rig_list_chains", { ik_rig_path: `${NS}/NoSuchIKRig` });
    expect(["success", "error"]).toContain(resp.status);
    await assertReady(ctx.mcp);
  });

  // ── editor utility ────────────────────────────────────────────────────────
  test("editor_console_exec", async () => {
    const r = await ctx.mcp.expect("editor_console_exec", { command: "stat none" });
    expect(typeof r === "object").toBeTruthy();
  });

  test("input_create", async () => {
    const path = `${NS}/IA_MCPTest`;
    await ensureAbsent(ctx.mcp, path);
    const r = await ctx.mcp.expect("input_create", {
      type: "action",
      name: "IA_MCPTest",
      path: NS,
      value_type: "boolean",
    });
    expect(r.asset_path).toBeTruthy();
    expect(String(r.class ?? "")).toContain("InputAction");
  });

  test("editor_live_coding_compile", async () => {
    // Live Coding hot-patches the running editor. Under headless -nullrhi the
    // module is usually unavailable → fast structured error; but a real compile
    // could block or drop the connection. Like the Python suite, treat any clean
    // outcome OR a transport hiccup as acceptable — the invariant is that the
    // editor stays interactive afterward (checked via the independent bridge).
    try {
      const resp = await ctx.mcp.command("editor_live_coding_compile", {});
      expect(["success", "error"]).toContain(resp.status);
    } catch {
      /* unavailable / in-progress / transport hiccup — acceptable */
    }
    await assertReady(ctx.bridge);
  });
});
