/**
 * Liveness smoke tests — prove the editor booted and the bridge answers.
 * Port of tests/integration/test_smoke.py.
 *
 * Mirrors the manual smoke sequence in docs/DEBUGGING.md: mcp_status -> ping ->
 * a read tool (asset_list).
 */

import { test, expect } from "bun:test";
import { editorSuite } from "../harness/suite.ts";

editorSuite("smoke", (ctx) => {
  test("mcp_status_ready", async () => {
    // mcp_status returns the bridge envelope unchanged; use .command (not .expect)
    // so the {status, result} shape is preserved for these assertions.
    const resp = await ctx.mcp.command("mcp_status", {});
    expect(resp.status).toEqual("success");
    const result = resp.result as Record<string, unknown>;
    expect(result.ready).toBe(true);
    expect(result.phase).toEqual("interactive");
  });

  test("ping", async () => {
    // ping is a bridge-internal liveness probe — there is NO standalone MCP tool
    // for it, so this one stays at the bridge level.
    const resp = await ctx.bridge.ping();
    expect(resp.status).toEqual("success");
  });

  test("list_assets_read_path", async () => {
    // A read-only command exercises the full game-thread dispatch path without
    // mutating anything. Root /Game is always listable, even in an empty project.
    // (list_assets is the bridge command; asset_list is a Python-only alias.)
    const result = await ctx.mcp.expect("asset_list", { directory_path: "/Game/", recursive: true });
    expect(result && typeof result === "object").toBeTruthy();
  });
});
