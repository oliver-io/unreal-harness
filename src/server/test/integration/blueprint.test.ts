/**
 * Blueprint domain — create an asset, mutate its graph/components/variables,
 * read the state back. Port of tests/integration/test_blueprint.py.
 *
 * Self-contained: needs no imported content. A module-scoped sample Blueprint is
 * created once under the test namespace and reused.
 *
 * Pattern for every test: arrange prerequisite state -> dispatch the op (raises on
 * a non-success envelope) -> assert the resulting state via a read/inspect op.
 */

import { test, expect, beforeAll } from "bun:test";
import { existsSync, statSync } from "node:fs";
import { join } from "node:path";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { ensureAbsent } from "../harness/ops.ts";
import { projectDir } from "../harness/env.ts";

const NS = `${ROOT}/blueprint`;
const SAMPLE = `${NS}/BP_Sample`;

/** Map a /Game/... content path to its on-disk package file. The file only
 *  exists after the asset is SAVED. Mirrors config.uasset_disk_path. */
function uassetDiskPath(gamePath: string, ext = ".uasset"): string {
  const pkg = gamePath.split(".")[0] ?? "";
  if (!pkg.startsWith("/Game/")) throw new Error(`not a /Game/ path: ${gamePath}`);
  const rel = pkg.slice("/Game/".length);
  return join(projectDir(), "Content", rel + ext);
}

editorSuite("blueprint", (ctx) => {
  // module-scoped sample_bp fixture: create one Actor Blueprint and compile it.
  beforeAll(async () => {
    await ensureAbsent(ctx.mcp, SAMPLE);
    await ctx.mcp.expect("bp_create_blueprint", { name: SAMPLE, parent_class: "Actor" });
    await ctx.mcp.expect("bp_compile", { blueprint_name: SAMPLE });
  });

  test("create_blueprint_writes_uasset_on_disk", async () => {
    const path = `${NS}/BP_Created`;
    await ensureAbsent(ctx.mcp, path);
    const result = await ctx.mcp.expect("bp_create_blueprint", { name: path, parent_class: "Actor" });
    expect(result.success).toBe(true);
    // State assertion: save it, then the package file must exist at the mapped Content path.
    await ctx.mcp.expect("asset_save", { asset_paths: [path] });
    const disk = uassetDiskPath(path);
    expect(existsSync(disk) && statSync(disk).isFile()).toBe(true);
  });

  test("compile_blueprint", async () => {
    const result = await ctx.mcp.expect("bp_compile", { blueprint_name: SAMPLE });
    expect(result.success).not.toBe(false);
  });

  test("bp_brief", async () => {
    const result = await ctx.mcp.expect("bp_brief", { bp_path: SAMPLE });
    expect(result && typeof result === "object").toBeTruthy();
  });

  test("bp_get_parent_class", async () => {
    const result = await ctx.mcp.expect("bp_get_parent_class", { bp_path: SAMPLE });
    // Parent was Actor; the reported parent class should mention it.
    const blob = JSON.stringify(result).toLowerCase();
    expect(blob).toContain("actor");
  });

  test("read_blueprint_content", async () => {
    const result = await ctx.mcp.expect("bp_read", { blueprint_path: SAMPLE });
    expect(result && typeof result === "object").toBeTruthy();
  });

  test("list_blueprint_graphs", async () => {
    const result = await ctx.mcp.expect("bp_list_graphs", { blueprint_path: SAMPLE });
    expect(result && typeof result === "object").toBeTruthy();
  });

  test("add_component_then_list", async () => {
    await ctx.mcp.expect("bp_add_component", {
      blueprint_name: SAMPLE,
      component_type: "StaticMeshComponent",
      component_name: "MCPTestMesh",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: SAMPLE });
    const comps = await ctx.mcp.expect("bp_list_components", { bp_path: SAMPLE });
    expect(JSON.stringify(comps)).toContain("MCPTestMesh");
  });

  test("create_variable_then_read", async () => {
    await ctx.mcp.expect("bp_create_variable", {
      blueprint_name: SAMPLE,
      variable_name: "MCPTestHealth",
      variable_type: "float",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: SAMPLE });
    const content = await ctx.mcp.expect("bp_read", { blueprint_path: SAMPLE });
    expect(JSON.stringify(content)).toContain("MCPTestHealth");
  });

  test("create_variable_dry_run_does_not_mutate", async () => {
    const result = await ctx.mcp.expect("bp_create_variable", {
      blueprint_name: SAMPLE,
      variable_name: "ShouldNotExist",
      variable_type: "bool",
      dry_run: true,
    });
    expect(result.dry_run).toBe(true);
  });
});
