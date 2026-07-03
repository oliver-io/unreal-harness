/**
 * Actor domain — spawn native actors into the transient level, mutate and
 * inspect them through the *legacy* actor commands, then assert via read-back.
 * Port of tests/integration/test_actor.py.
 *
 * Exercises the legacy spawn_actor / find_actors_by_name / actor_get_in_level /
 * actor_inspect commands plus the per-actor mutators (actor_set_transform,
 * actor_set_property, actor_delete) and the material-info / gamemode helpers.
 *
 * Native actors live in the unsaved transient level, so editor-quit is a full
 * reset — no on-disk cleanup needed. Spawns are made re-runnable by deleting any
 * prior actor of the same name first (spawn_actor refuses a name collision).
 */

import { test, expect, beforeAll } from "bun:test";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { ensureAbsent, assertReady } from "../harness/ops.ts";
import type { Commandable } from "../harness/ops.ts";
import { covers } from "../harness/coverage.ts";

// A StaticMeshActor needs a mesh for get_actor_material_info to report a slot;
// the engine cube is always present and asset-free for the test project.
const CUBE_MESH = "/Engine/BasicShapes/Cube.Cube";
const NS = `${ROOT}/actor`;

/** Arrange helper (mirrors ops.ensureAbsent for assets): drop any actor of this
 *  name so the following spawn_actor won't hit a name collision. */
async function deleteActorIfPresent(bridge: Commandable, name: string): Promise<void> {
  try {
    await bridge.command("actor_delete", { name });
  } catch {
    /* ignore */
  }
}

/** Idempotent spawn of a native actor; returns the spawn_actor result
 *  (ActorToJsonObject: name, class, location[], rotation[], scale[]). */
async function spawn(
  bridge: Commandable,
  name: string,
  actorType = "StaticMeshActor",
  extra: Record<string, unknown> = {},
): Promise<Record<string, unknown>> {
  await deleteActorIfPresent(bridge, name);
  const params: Record<string, unknown> = { name, type: actorType, ...extra };
  return bridge.expect("spawn_actor", params);
}

editorSuite("actor", (ctx) => {
  // One shared StaticMeshActor (with the engine cube) for the read-only and
  // non-destructive tests. Lives in the transient level; the editor-quit reset
  // cleans it up. Spawns at the BRIDGE level: the legacy `spawn_actor` command
  // has no standalone MCP tool (only `actor_spawn`).
  let smActor: string;

  beforeAll(async () => {
    smActor = "MCPTest_SM_Shared";
    await spawn(ctx.bridge, smActor, "StaticMeshActor", { static_mesh: CUBE_MESH });
  });

  test("spawn_actor_then_find_and_list", async () => {
    // A real spawn must be addressable by name and present in the level list.
    // Stays at the BRIDGE level: the legacy `spawn_actor` command this test
    // exercises has no standalone MCP tool (only `actor_spawn`).
    const name = "MCPTest_SM_Spawn";
    const spawned = await spawn(ctx.bridge, name, "StaticMeshActor", { static_mesh: CUBE_MESH });
    expect(spawned.name).toEqual(name);
    expect(String(spawned.class).endsWith("StaticMeshActor")).toBe(true);

    const found = await ctx.bridge.expect("find_actors_by_name", { pattern: name });
    const names = ((found.actors as any[]) ?? []).map((a) => a.name);
    expect(names).toContain(name);

    const inLevel = await ctx.bridge.expect("actor_get_in_level", {});
    const allNames = ((inLevel.actors as any[]) ?? []).map((a) => a.name);
    expect(allNames).toContain(name);
  });

  test("actor_inspect_reports_class_and_transform", async () => {
    const result = await ctx.mcp.expect("actor_inspect", { name: smActor });
    expect(result.name).toEqual(smActor);
    // actor_inspect reports the full class path (/Script/Engine.StaticMeshActor).
    expect(String(result.class)).toContain("StaticMeshActor");
    // Transform is an object {x,y,z}; components are enumerated.
    const loc = result.location as Record<string, unknown>;
    expect(loc.x).toBeDefined();
    expect(loc.y).toBeDefined();
    expect(loc.z).toBeDefined();
    expect(result.components).toBeDefined();
  });

  test("set_actor_transform_then_inspect", async () => {
    // Move the actor, then prove the new location through a different command.
    await ctx.mcp.expect("actor_set_transform", {
      name: smActor,
      location: [120.0, 240.0, 360.0],
    });
    const result = await ctx.mcp.expect("actor_inspect", { name: smActor });
    const loc = result.location as Record<string, number>;
    expect(Math.abs(loc.x! - 120.0) <= 1.0).toBe(true);
    expect(Math.abs(loc.y! - 240.0) <= 1.0).toBe(true);
    expect(Math.abs(loc.z! - 360.0) <= 1.0).toBe(true);
  });

  test("set_actor_transform_dry_run_does_not_mutate", async () => {
    // dry_run validates and emits a transforms_changed diff without mutating.
    const before = (await ctx.mcp.expect("actor_inspect", { name: smActor }))
      .location as Record<string, number>;
    // Guard: the proposed location must differ from the current one, or the
    // "did not move" observation below would be vacuous.
    expect(
      Math.abs(before.x! - 1.0) < 0.1 &&
        Math.abs(before.y! - 2.0) < 0.1 &&
        Math.abs(before.z! - 3.0) < 0.1,
    ).toBe(false);

    const result = await ctx.mcp.expect("actor_set_transform", {
      name: smActor,
      location: [1.0, 2.0, 3.0],
      dry_run: true,
    });
    expect(result.dry_run).toBe(true);
    const changed = (result.diff as any).transforms_changed;
    expect(changed[0].name).toEqual(smActor);

    // Independent readback: the actor must NOT have moved.
    const after = (await ctx.mcp.expect("actor_inspect", { name: smActor }))
      .location as Record<string, number>;
    for (const axis of ["x", "y", "z"] as const) {
      expect(Math.abs(after[axis]! - before[axis]!) <= 0.01).toBe(true);
    }
  });

  test("actor_set_property_then_readback", async () => {
    // Reflective write of a leaf on a placed actor, proven by re-reading the
    // stored value through a separate invocation — not the mutator's own echo.
    // actor_inspect does not export component leaf properties, so the
    // independent readback is actor_set_property's dry-run before-value:
    // dry_run resolves and exports the CURRENT value without mutating
    // (properties_changed diff, B4 convention).
    const result = await ctx.mcp.expect("actor_set_property", {
      name: smActor,
      property: "StaticMeshComponent.BoundsScale",
      value: 2.0,
    });
    expect(result.success).toBe(true);
    // Default BoundsScale is 1.0; the exported 'after' text must reflect 2.0.
    expect(String(result.after)).toContain("2");
    expect(result.before).not.toEqual(result.after);

    // Independent readback: a fresh dry-run resolve of the same leaf must
    // report the persisted 2.0 as its before-value (and not mutate anything).
    const probe = await ctx.mcp.expect("actor_set_property", {
      name: smActor,
      property: "StaticMeshComponent.BoundsScale",
      value: 4.0,
      dry_run: true,
    });
    expect(probe.dry_run).toBe(true);
    const entry = (probe.diff as any).properties_changed[0];
    expect(Math.abs(parseFloat(String(entry.before)) - 2.0) < 0.001).toBe(true);
  });

  test("delete_actor_then_absent", async () => {
    // Delete a dedicated actor (not the shared one) and prove it's gone. The
    // setup spawn uses the legacy `spawn_actor` command (no MCP tool) via the
    // bridge; the tools under test — actor_delete and find_actors_by_name — run
    // through the real MCP server.
    const name = "MCPTest_SM_ToDelete";
    await spawn(ctx.bridge, name, "StaticMeshActor", { static_mesh: CUBE_MESH });

    const result = await ctx.mcp.expect("actor_delete", { name });
    expect(result.deleted_actor).toBeDefined();

    const found = await ctx.mcp.expect("find_actors_by_name", { pattern: name });
    const names = ((found.actors as any[]) ?? []).map((a) => a.name);
    expect(names).not.toContain(name);
  });

  test("get_actor_material_info", async () => {
    // The shared actor carries the engine cube, so it must report >=1 slot.
    const result = await ctx.mcp.expect("mesh_get_actor_material_info", { actor_name: smActor });
    expect(result.actor_name).toEqual(smActor);
    expect((result.total_slots as number) >= 1).toBe(true);
    const slots = result.material_slots as any[];
    expect(slots && slots.length > 0).toBeTruthy();
    expect(slots[0].material_path).toBeDefined();
  });

  test("set_mesh_material_color", async () => {
    // GAP-009: mesh_set_mesh_material_color targets a Blueprint SCS component
    // *template* and now REFUSES the dynamic-instance path (a runtime MID baked into
    // a saved template corrupts level saves). It must return a structured
    // feature_disabled error directing to the saved-asset path.
    const bp = `${NS}/BP_MeshColor`;
    await ensureAbsent(ctx.mcp, bp);
    await ctx.mcp.expect("bp_create_blueprint", { name: bp, parent_class: "Actor" });
    await ctx.mcp.expect("bp_add_component", {
      blueprint_name: bp,
      component_type: "StaticMeshComponent",
      component_name: "Mesh",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: bp });

    // Use call() (not expect()) — the refusal is intentional, not a failure.
    const result = await ctx.mcp.call("mesh_set_mesh_material_color", {
      blueprint_name: bp,
      component_name: "Mesh",
      color: [1.0, 0.25, 0.0, 1.0],
      material_path: "/Engine/BasicShapes/BasicShapeMaterial",
      parameter_name: "BaseColor",
    });
    expect(result.status).toBe("error");
    expect(result.error_code).toBe("feature_disabled");
    expect(JSON.stringify(result)).toContain("material_create_instance");
  });

  /** Independent reader for AWorldSettings::DefaultGameMode on an arbitrary
   *  saved level. level_inspect only surfaces the WorldSettings actor's path
   *  and class (not its properties), so the sanctioned `py` console escape
   *  hatch is the observer: load the UWorld and export the class path. */
  async function readGamemodeOverride(levelPath: string): Promise<string> {
    const name = levelPath.split("/").pop();
    const probe = await ctx.mcp.expect("editor_console_exec", {
      command:
        "py import unreal; " +
        `w = unreal.load_object(None, '${levelPath}.${name}'); ` +
        "ws = w.get_world_settings(); " +
        "gm = ws.get_editor_property('default_game_mode'); " +
        "print('MCPTEST_GMO=' + (gm.get_path_name() if gm else 'None'))",
    });
    const m = /MCPTEST_GMO=(\S+)/.exec(String(probe.output ?? ""));
    expect(m).toBeTruthy();
    return m![1]!;
  }

  test("set_world_gamemode_override", async () => {
    // Bind a level's World Settings GameModeOverride, prove it via an
    // independent py readback of the saved world's DefaultGameMode, and
    // RESTORE the prior override (this targets a real level of the host
    // project — the original setting must survive the test). Requires a saved
    // World asset — skip cleanly if the project has none.
    const listing = await ctx.mcp.expect("asset_list", {
      directory_path: "/Game/",
      recursive: true,
      class_filter: "World",
    });
    const data = (listing.data as Record<string, unknown>) ?? listing;
    const worlds = (((data.assets as any[]) ?? []) as any[]).filter((a) => a.class === "World");
    if (worlds.length === 0) {
      console.log("SKIP: no saved World/level asset in the test project to target");
      return;
    }

    const levelPath = String(worlds[0].path).split(".")[0]!;
    const original = await readGamemodeOverride(levelPath);
    try {
      const result = await ctx.mcp.expect("level_set_gamemode_override", {
        level_path: levelPath,
        gamemode_class: "/Script/Engine.GameModeBase",
      });
      expect(result.success).toBe(true);
      expect(String(result.gamemode_class ?? "")).toContain("GameMode");

      // Independent readback: the world's DefaultGameMode is the set class.
      expect(await readGamemodeOverride(levelPath)).toEqual("/Script/Engine.GameModeBase");
    } finally {
      // Restore the prior override ('' clears it) — never leave the host
      // project's level pointing at the test gamemode.
      await ctx.mcp.command("level_set_gamemode_override", {
        level_path: levelPath,
        gamemode_class: original === "None" ? "" : original,
      });
    }
    expect(await readGamemodeOverride(levelPath)).toEqual(original);
    await assertReady(ctx.mcp);
  });
});

// Separate suite: the actor_spawn_physics composite is fully self-arranging
// (it builds its own throwaway Blueprint), so it must not depend on the shared
// StaticMeshActor fixture above — whose origin spawn fails in levels with
// collision at (0,0,0).
editorSuite("actor_spawn_physics", (ctx) => {
  covers("actor_spawn_physics");
  test("actor_spawn_physics_composite_spawns_simulating_actor", async () => {
    // SERVER-LOCAL composite under test (src/server/src/domains/actor.ts):
    // actor_spawn_physics fans out over bp_create_blueprint → bp_add_component
    // → mesh_set_static_mesh_properties → physics_set_properties → bp_compile
    // → spawn_blueprint_actor, and returns the final spawn ENVELOPE
    // ({status:"success", result:ActorToJsonObject}) — so ctx.mcp.expect()
    // unwraps it like any bridge tool. Invoked through the real MCP client
    // (protocol → registry → the composite's handler), never the raw bridge.
    const base = `MCPTest_SpawnPhysics_${process.pid}`;
    // bp_create_blueprint resolves the composite's short name `<base>_BP`
    // against /Game/Blueprints — that asset is ours to pre-clear and delete.
    const bpPath = `/Game/Blueprints/${base}_BP`;

    // Arrange: nothing of ours may pre-exist (idempotent re-runs). The pid
    // suffix keeps concurrent agents apart; the sweep clears stale leftovers
    // from a previous crashed run of the same pid.
    await ensureAbsent(ctx.mcp, bpPath);
    const stale = await ctx.mcp.expect("find_actors_by_name", { pattern: base });
    for (const a of (stale.actors as any[]) ?? []) {
      await deleteActorIfPresent(ctx.mcp, String(a.name));
    }

    let spawnedName: string | undefined;
    try {
      // Act — through the ACTUAL actor_spawn_physics ToolDef handler.
      const spawned = await ctx.mcp.expect("actor_spawn_physics", {
        name: base,
        location: [0.0, 0.0, 3000.0],
        mass: 5.0,
        simulate_physics: true,
      });
      // spawn_blueprint_actor answers with ActorToJsonObject: the instance
      // FName derives from the generated class (<base>_BP_C_N).
      spawnedName = String(spawned.name);
      expect(spawnedName).toContain(base);
      expect(String(spawned.class)).toContain(`${base}_BP_C`);

      // Observe 1 (different primitive): the actor exists in the level.
      const found = await ctx.mcp.expect("find_actors_by_name", { pattern: base });
      const names = ((found.actors as any[]) ?? []).map((a) => a.name);
      expect(names).toContain(spawnedName);

      // Observe 2 (different primitive): actor_inspect sees the composite's
      // "Mesh" StaticMeshComponent carrying the default engine cube.
      const inspected = await ctx.mcp.expect("actor_inspect", { name: spawnedName });
      expect(String(inspected.class)).toContain(`${base}_BP_C`);
      const comps = (inspected.components as any[]) ?? [];
      const mesh = comps.find((c) => c.name === "Mesh");
      expect(mesh).toBeDefined();
      expect(String(mesh.class)).toContain("StaticMeshComponent");

      // Observe 3: simulate-physics landed on the spawned INSTANCE. There is
      // no dedicated physics read for a generic actor, so read the leaf via
      // actor_set_property dry_run — it resolves + exports `before` without
      // mutating (properties_changed diff, doc-13 convention).
      const probe = await ctx.mcp.expect("actor_set_property", {
        name: spawnedName,
        property: "Mesh.BodyInstance.bSimulatePhysics",
        value: true,
        dry_run: true,
      });
      expect(probe.dry_run).toBe(true);
      const entry = (probe.diff as any).properties_changed[0];
      expect(entry.before).toBe("True");

      // The mass override from physics_set_properties, same read path.
      const massProbe = await ctx.mcp.expect("actor_set_property", {
        name: spawnedName,
        property: "Mesh.BodyInstance.MassInKgOverride",
        value: 5.0,
        dry_run: true,
      });
      const massEntry = (massProbe.diff as any).properties_changed[0];
      expect(Math.abs(parseFloat(String(massEntry.before)) - 5.0) < 0.001).toBe(true);
    } finally {
      // Self-cleaning even on failure: drop the actor and the throwaway BP.
      if (spawnedName) await deleteActorIfPresent(ctx.mcp, spawnedName);
      await ensureAbsent(ctx.mcp, bpPath);
    }
  });
});
