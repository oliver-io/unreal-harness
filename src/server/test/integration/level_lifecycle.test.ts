/**
 * Level lifecycle: level_new -> level_save_as -> level_save -> level_load.
 * Parity twin of tests/integration/test_level_lifecycle.py.
 *
 * HAZARD gate — mirrors the pytest module's launch-mode-only skip. The C++
 * handlers (MCPLevelCommands.cpp) switch worlds WITHOUT saving the outgoing
 * one (level_new: bSaveExisting=false; level_load: LoadMap, no save prompt),
 * so switching levels DISCARDS the open map's unsaved changes. The bun editor
 * tier always attaches to whatever editor is on :55557 — which on a dev box is
 * the SHARED editor carrying other agents' WIP — so these tests additionally
 * require UE_MCP_FIXTURE_EDITOR=1, an explicit "the live editor is a
 * disposable fixture instance (tests/fixtures/TestProject), not the shared
 * dev editor" attestation (the analog of pytest's launch mode).
 *
 * Every test restores the original open level in finally, then deletes the
 * saved test map.
 */

import { test, expect } from "bun:test";
import { existsSync, statSync } from "node:fs";
import { join } from "node:path";
import { editorSuite, NS, type Ctx } from "../harness/suite.ts";
import { projectDir } from "../harness/env.ts";

/** Set ONLY when the live editor is a disposable fixture instance. */
const FIXTURE: boolean = process.env.UE_MCP_FIXTURE_EDITOR === "1";

const TEST_MAP = `${NS}/levels/L_MCPLifecycle`;
const MISSING_MAP = `${NS}/levels/L_DoesNotExist`;
const POINT_LIGHT = "/Script/Engine.PointLight";

/** /Game/... package path -> on-disk .umap (fixture project's Content/). */
const umapDiskPath = (gamePath: string): string =>
  join(projectDir(), "Content", gamePath.replace(/^\/Game\//, "") + ".umap");

/** The open level's package path via level_inspect (independent readback). */
async function currentPackage(ctx: Ctx): Promise<string> {
  const info = await ctx.mcp.expect("level_inspect", {});
  return String(info["path"]).split(".")[0]!;
}

/** Snapshot the open level, run the test body, ALWAYS restore, then delete
 *  the test map. Best-effort teardown — it must never fail the run. */
async function withLevelGuard(ctx: Ctx, body: () => Promise<void>): Promise<void> {
  const original = await currentPackage(ctx);
  try {
    await body();
  } finally {
    let restored = false;
    try {
      if (original.startsWith("/Game/")) {
        const resp = await ctx.mcp.call("level_load", { package_path: original });
        restored = resp.status === "success";
      }
    } catch {
      /* best effort */
    }
    if (!restored) {
      try {
        await ctx.mcp.call("level_new", {});
      } catch {
        /* best effort */
      }
    }
    try {
      await ctx.mcp.call("asset_delete", { asset_path: TEST_MAP, force: true });
    } catch {
      /* best effort */
    }
  }
}

editorSuite("level_lifecycle", (ctx) => {
  test.skipIf(!FIXTURE)("test_level_new_blank_replaces_world_with_transient_map", async () => {
    await withLevelGuard(ctx, async () => {
      const before = await currentPackage(ctx);

      const created: any = await ctx.mcp.expect("level_new", {});
      expect(created.created).toBe(true);
      const newPkg = String(created.package_path);
      expect(newPkg).not.toEqual(before); // level_new must replace the open world
      expect(newPkg.startsWith("/Temp/")).toBe(true); // transient until save_as

      // Independent readback: the editor's active world actually switched.
      const info: any = await ctx.mcp.expect("level_inspect", {});
      expect(String(info.path).split(".")[0]).toEqual(newPkg);
      expect(info.name).toEqual(created.map_name);
    });
  });

  test.skipIf(!FIXTURE)("test_level_save_refuses_transient_untitled_level", async () => {
    await withLevelGuard(ctx, async () => {
      await ctx.mcp.expect("level_new", {});

      // A /Temp/ untitled world has no on-disk home — level_save must refuse.
      const resp: any = await ctx.mcp.call("level_save", {});
      expect(resp.status).toBe("error");
      expect(resp.error_code).toBe("invalid_path");
    });
  });

  test.skipIf(!FIXTURE)("test_level_save_as_save_and_load_roundtrip", async () => {
    await withLevelGuard(ctx, async () => {
      await ctx.mcp.expect("level_new", {});

      // save_as: the transient world lands on disk at the named /Game/ path...
      const saved: any = await ctx.mcp.expect("level_save_as", { package_path: TEST_MAP });
      expect(saved.saved).toBe(true);
      expect(saved.package_path).toEqual(TEST_MAP);
      const umap = umapDiskPath(TEST_MAP);
      expect(existsSync(umap)).toBe(true);
      expect(statSync(umap).size).toBeGreaterThan(0);
      // ...and becomes the ACTIVE level (independent readback).
      expect(await currentPackage(ctx)).toEqual(TEST_MAP);

      // Mutate, then level_save: the change must reach the disk package.
      const spawned: any = await ctx.mcp.expect("actor_spawn", {
        class_path: POINT_LIGHT,
        name: "MCPTest_LevelPersist",
      });
      const spawnedName = spawned.actor.name;
      const mtimeBefore = statSync(umap).mtimeMs;
      const resaved: any = await ctx.mcp.expect("level_save", {});
      expect(resaved.saved).toBe(true);
      expect(resaved.package_path).toEqual(TEST_MAP);
      expect(statSync(umap).mtimeMs).not.toEqual(mtimeBefore); // .umap really rewrote

      // Switch away, then load back from disk.
      await ctx.mcp.expect("level_new", {});
      expect(await currentPackage(ctx)).not.toEqual(TEST_MAP);
      const loaded: any = await ctx.mcp.expect("level_load", { package_path: TEST_MAP });
      expect(loaded.loaded).toBe(true);
      expect(loaded.package_path).toEqual(TEST_MAP);
      expect(await currentPackage(ctx)).toEqual(TEST_MAP);

      // Deep observation: the saved actor survived the disk round-trip.
      const found: any = await ctx.mcp.expect("actor_query", {
        name_pattern: "MCPTest_LevelPersist",
      });
      const names = (found.actors ?? []).map((a: Record<string, unknown>) => a["name"]);
      expect(names).toContain(spawnedName);
    });
  });

  test.skipIf(!FIXTURE)("test_level_load_missing_map_is_asset_not_found", async () => {
    await withLevelGuard(ctx, async () => {
      const before = await currentPackage(ctx);

      // The handler pre-validates existence before touching LoadMap.
      const resp: any = await ctx.mcp.call("level_load", { package_path: MISSING_MAP });
      expect(resp.status).toBe("error");
      expect(resp.error_code).toBe("asset_not_found");
      expect(await currentPackage(ctx)).toEqual(before); // failed load must not switch
    });
  });
});
