/**
 * Landscape read-only ops: landscape_inspect / landscape_list_layers /
 * landscape_read_heightmap. Parity twin of tests/integration/test_landscape.py
 * (the pytest module carries the @covers ledger — these are bridge ops).
 *
 * Arrange strategy — a BARE ALandscape spawned through the typed actor_spawn
 * primitive. A bare proxy has no landscape components, which is exactly what
 * the harness can arrange: neither the typed surface nor the UE 5.7 Python
 * API can create landscape COMPONENTS (landscape_import_heightmap_from_
 * render_target requires them to already exist). Value-bearing heightmap /
 * assigned-layer positive paths are DEFERRED (docs/loops/tests/TASKS.md).
 *
 * Attach-safe: one actor spawned into (and always deleted from) the open
 * level; nothing is saved. Live probe 2026-07-02: spawn -> observe -> delete
 * leaves zero residue.
 */

import { test, expect } from "bun:test";
import { editorSuite, type Ctx } from "../harness/suite.ts";

const LANDSCAPE_CLASS = "/Script/Landscape.Landscape";
const ACTOR = "MCPTest_Landscape";
const MISSING = "MCPTest_NoSuchLandscape";
// Far corner: irrelevant to a component-less landscape, but keeps the marker
// actor away from anything real in a shared attached level.
const SPAWN_LOC = { x: 100000.0, y: 100000.0, z: -10000.0 };

/** Spawn one bare ALandscape, run the body, ALWAYS delete it.
 *  Passes (fname, label): after a same-name delete the FName stays reserved
 *  until GC, so a respawn can come back suffixed (`_0`) while the editor
 *  LABEL stays clean — handlers match either but echo the label. */
async function withLandscape(
  ctx: Ctx,
  body: (name: string, label: string) => Promise<void>,
): Promise<void> {
  try {
    await ctx.mcp.call("actor_delete", { name: ACTOR }); // idempotent re-runs
  } catch {
    /* absent is fine */
  }
  const spawned: any = await ctx.mcp.expect("actor_spawn", {
    class_path: LANDSCAPE_CLASS,
    name: ACTOR,
    location: SPAWN_LOC,
  });
  const name = String(spawned.actor.name);
  const label = String(spawned.actor.label);
  try {
    await body(name, label);
  } finally {
    try {
      await ctx.mcp.call("actor_delete", { name });
    } catch {
      /* best effort */
    }
  }
}

editorSuite("landscape", (ctx) => {
  test("landscape_inspect_reports_spawned_landscape", async () => {
    await withLandscape(ctx, async (name, label) => {
      // Find-by-name: exactly the spawned proxy, class label + arranged transform.
      const result: any = await ctx.mcp.expect("landscape_inspect", { actor_name: name });
      expect(result.count).toBe(1);
      const entry = result.landscapes[0];
      expect(entry.name).toBe(label); // handler echoes the actor label
      expect(entry.internal_name).toBe(name);
      expect(entry.class).toBe("Landscape");
      expect(Math.abs(entry.location.x - SPAWN_LOC.x) <= 1.0).toBe(true);
      expect(Math.abs(entry.location.z - SPAWN_LOC.z) <= 1.0).toBe(true);
      // A bare proxy has no components: quad config zeroed, no extent block.
      expect(entry.component_size_quads).toBe(0);
      expect(entry.extent_quads).toBeUndefined();

      // Enumeration must include it (>= tolerates real landscapes in a
      // shared attached level).
      const listing: any = await ctx.mcp.expect("landscape_inspect", {});
      expect(listing.count).toBeGreaterThanOrEqual(1);
      const names = listing.landscapes.map((e: any) => e.name);
      expect(names).toContain(label);
    });
  });

  test("landscape_inspect_unknown_name_is_actor_not_found", async () => {
    // A non-empty actor_name never falls back to "first landscape".
    const resp: any = await ctx.mcp.call("landscape_inspect", { actor_name: MISSING });
    expect(resp.status).toBe("error");
    expect(resp.error_code).toBe("actor_not_found");
  });

  test("landscape_list_layers_component_less_landscape_is_empty", async () => {
    await withLandscape(ctx, async (name, label) => {
      // Success path: resolves the proxy; component-less => empty layer set.
      const result: any = await ctx.mcp.expect("landscape_list_layers", { actor_name: name });
      expect(result.actor).toBe(label); // handler echoes the actor label
      expect(result.count).toBe(0);
      expect(result.layers).toEqual([]);
    });
  });

  test("landscape_list_layers_unknown_name_is_actor_not_found", async () => {
    const resp: any = await ctx.mcp.call("landscape_list_layers", { actor_name: MISSING });
    expect(resp.status).toBe("error");
    expect(resp.error_code).toBe("actor_not_found");
  });

  test("landscape_read_heightmap_component_less_landscape_is_invalid_argument", async () => {
    await withLandscape(ctx, async (name) => {
      // The no-ULandscapeInfo gate: refuse, don't crash or fake an empty grid.
      const resp: any = await ctx.mcp.call("landscape_read_heightmap", { actor_name: name });
      expect(resp.status).toBe("error");
      expect(resp.error_code).toBe("invalid_argument");
      expect(String(resp.error ?? "")).toContain("ULandscapeInfo");
    });
  });

  test("landscape_read_heightmap_unknown_name_is_actor_not_found", async () => {
    const resp: any = await ctx.mcp.call("landscape_read_heightmap", { actor_name: MISSING });
    expect(resp.status).toBe("error");
    expect(resp.error_code).toBe("actor_not_found");
  });
});
