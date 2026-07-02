/**
 * The canonical mutate -> inspect -> assert loop. Port of
 * tests/integration/test_actor_level.py.
 *
 * Spawns a native PointLight (no asset dependency, so it works in an empty
 * project) into the unsaved transient level, then proves it landed by inspecting
 * the level. Nothing is saved to disk, so editor-quit is a full reset — no
 * per-test cleanup needed.
 */

import { test, expect } from "bun:test";
import { editorSuite } from "../harness/suite.ts";
import type { TestClient } from "../harness/mcpClient.ts";

const POINT_LIGHT = "/Script/Engine.PointLight";

async function persistentActorCount(bridge: TestClient): Promise<number> {
  const info: any = await bridge.expect("level_inspect", {});
  return Number(info["persistent_level"]["actor_count"]);
}

editorSuite("actor_level", (ctx) => {
  test("test_actor_spawn_dry_run_does_not_mutate", async () => {
    // dry_run validates without spawning — the actor count must be unchanged.
    const before = await persistentActorCount(ctx.mcp);
    // TOOL kwarg is class_path (the tool layer maps it to the bridge 'class' param).
    const result: any = await ctx.mcp.expect("actor_spawn", { class_path: POINT_LIGHT, dry_run: true });
    expect(result.dry_run).toBe(true);
    expect(String(result["diff"]["actors_added"][0]["class"]).endsWith("PointLight")).toBe(true);
    const after = await persistentActorCount(ctx.mcp);
    expect(after).toEqual(before); // dry_run must not change the level
  });

  test("test_actor_spawn_then_inspect", async () => {
    // A real spawn must increment the level's actor count and be addressable.
    const before = await persistentActorCount(ctx.mcp);

    // TOOL kwarg is class_path (the tool layer maps it to the bridge 'class' param).
    const spawned: any = await ctx.mcp.expect("actor_spawn", {
      class_path: POINT_LIGHT,
      name: "MCPTest_PointLight",
      location: { x: 100.0, y: 200.0, z: 300.0 },
    });
    expect(spawned.success).toBe(true);
    expect(String(spawned["actor"]["class"]).endsWith("PointLight")).toBe(true);
    const spawnedName = spawned["actor"]["name"];

    const after = await persistentActorCount(ctx.mcp);
    expect(after).toEqual(before + 1); // real spawn must add exactly one actor

    // And it must be queryable by name through a different command path.
    const found: any = await ctx.mcp.expect("actor_query", { name_pattern: "MCPTest_PointLight" });
    const names = (found["actors"] ?? []).map((a: Record<string, unknown>) => a["name"]);
    expect(names).toContain(spawnedName);
  });
});
