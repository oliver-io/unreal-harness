/**
 * Foliage read-only inspection: foliage_inspect. Parity twin of
 * tests/integration/test_foliage.py (the pytest module carries the @covers
 * ledger — this is a bridge op).
 *
 * Arrange — ZERO C++, via the py escape hatch (recipe proven live 2026-07-03):
 * a TRANSIENT FoliageType_InstancedStaticMesh pointing at the engine Cube +
 * the static BlueprintCallable unreal.InstancedFoliageActor.add_instances
 * (creates the level's canonical IFA on demand). Nothing is saved.
 *
 * Teardown (proven live): add_instances COPIES the transient type into the
 * IFA, so remove_all_instances must be fed the IFA's OWN registered type from
 * get_used_foliage_types() — the transient handle silently removes nothing.
 * The emptied IFA is then deleted (an IFA is NOT a LandscapeProxy; the
 * docs/BUGS.md bare-ALandscape hazard does not apply), restoring the exact
 * {ifa_count:0, total_types:0, total_instances:0} baseline. Shared-level
 * safety: only Cube-mesh types are removed, only IFAs that did not pre-exist
 * are deleted, and the whole arrange is skipped if the level already has
 * Cube foliage (never touch someone's real content).
 *
 * The unknown-mode error path runs over the RAW bridge: the server-side zod
 * enum refuses a bogus mode at the MCP layer, so the C++ gate is reachable
 * only on the wire.
 */

import { test, expect } from "bun:test";
import { editorSuite, type Ctx } from "../harness/suite.ts";

const CUBE = "/Engine/BasicShapes/Cube.Cube";
const MISSING_TYPE = "MCPTest_NoSuchFoliageType";
const IFA_CLASS = "/Script/Foliage.InstancedFoliageActor";

// Far corner, away from anything real in a shared attached level.
const LOCS: [number, number, number][] = [
  [100000.0, 100000.0, -5000.0],
  [100100.0, 100000.0, -5000.0],
  [100200.0, 100000.0, -5000.0],
];

const ARRANGE_PY =
  "py import unreal; " +
  "w = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world(); " +
  "t = unreal.FoliageType_InstancedStaticMesh(); " +
  `t.set_editor_property('mesh', unreal.load_asset('${CUBE}')); ` +
  "ts = [" +
  LOCS.map(([x, y, z]) => `unreal.Transform(location=[${x}, ${y}, ${z}])`).join(", ") +
  "]; " +
  "unreal.InstancedFoliageActor.add_instances(w, t, ts)";

// Remove ONLY Cube-mesh foliage types, via each IFA's OWN registered types.
const TEARDOWN_PY =
  "py import unreal; " +
  "s = unreal.get_editor_subsystem(unreal.EditorActorSubsystem); " +
  "w = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world(); " +
  "ifas = [a for a in s.get_all_level_actors() if isinstance(a, unreal.InstancedFoliageActor)]; " +
  "[unreal.InstancedFoliageActor.remove_all_instances(w, t) " +
  " for ifa in ifas for t in ifa.get_used_foliage_types() " +
  ` if t.get_editor_property('mesh') and t.get_editor_property('mesh').get_path_name() == '${CUBE}']`;

async function ifaNames(ctx: Ctx): Promise<Set<string>> {
  const listing: any = await ctx.mcp.expect("actor_query", { class_filter: IFA_CLASS });
  return new Set((listing.actors as any[]).map((a) => String(a.name)));
}

function cubeEntry(inspectResult: any): any | null {
  for (const e of inspectResult?.types ?? []) if (e.mesh === CUBE) return e;
  return null;
}

/** Arrange three Cube foliage instances, run the body, ALWAYS tear down.
 *  Passes the pre-arrange foliage_inspect baseline. Returns without running
 *  the body (the pytest twin's skip) if the level already has Cube foliage —
 *  the teardown could not distinguish ours from real content. */
async function withFoliage(ctx: Ctx, body: (baseline: any) => Promise<void>): Promise<void> {
  const baseline: any = await ctx.mcp.expect("foliage_inspect", {});
  if (cubeEntry(baseline) !== null) {
    console.warn("foliage: level already has Cube-mesh foliage — skipping arrange-based assertions");
    return;
  }
  const preIfas = await ifaNames(ctx);
  await ctx.mcp.expect("editor_console_exec", { command: ARRANGE_PY });
  try {
    await body(baseline);
  } finally {
    // Teardown must never fail the run; each step is independent.
    try {
      await ctx.mcp.call("editor_console_exec", { command: TEARDOWN_PY });
    } catch {
      /* best effort */
    }
    try {
      for (const name of await ifaNames(ctx)) {
        if (!preIfas.has(name)) await ctx.mcp.call("actor_delete", { name });
      }
    } catch {
      /* best effort */
    }
  }
}

editorSuite("foliage", (ctx) => {
  test("foliage_inspect_types_reports_arranged_type", async () => {
    await withFoliage(ctx, async (baseline) => {
      // mode='types' (default): per-type aggregate of the placed foliage.
      const result: any = await ctx.mcp.expect("foliage_inspect", {});
      expect(result.mode).toBe("types");
      expect(result.ifa_count).toBeGreaterThanOrEqual(1);
      expect(result.total_types).toBe(baseline.total_types + 1);
      expect(result.total_instances).toBe(baseline.total_instances + LOCS.length);

      const entry = cubeEntry(result);
      expect(entry).not.toBeNull();
      expect(entry.identity).toBe(CUBE); // ISM identity is the mesh path
      expect(entry.display_name).toBe("Cube");
      expect(entry.instance_count).toBe(LOCS.length);
      // Key-property block is present and sane (UFoliageType defaults).
      expect(entry.density).toBe(100);
      expect(entry.radius).toBe(0);
      expect(typeof entry.align_to_normal).toBe("boolean");
      expect(typeof entry.random_yaw).toBe("boolean");
      expect(Object.keys(entry.cull_distance).sort()).toEqual(["max", "min"]);
    });
  });

  test("foliage_inspect_instances_pages_exact_transforms", async () => {
    await withFoliage(ctx, async () => {
      // limit 2 → returned 2 + truncated; offset 2 → the final one.
      const page1: any = await ctx.mcp.expect("foliage_inspect", {
        mode: "instances",
        foliage_type: CUBE,
        limit: 2,
      });
      expect(page1.mode).toBe("instances");
      expect(page1.foliage_type).toBe(CUBE);
      expect(page1.total_instances).toBe(LOCS.length);
      expect(page1.returned).toBe(2);
      expect(page1.instances.length).toBe(2);
      expect(page1.truncated).toBe(true);

      const page2: any = await ctx.mcp.expect("foliage_inspect", {
        mode: "instances",
        foliage_type: CUBE,
        limit: 2,
        offset: 2,
      });
      expect(page2.returned).toBe(1);
      expect(page2.truncated).toBe(false);

      const got: any[] = [...page1.instances, ...page2.instances];
      expect(got.map((i) => i.index)).toEqual([0, 1, 2]);
      const locs = got
        .map((i) => [i.location.x, i.location.y, i.location.z] as [number, number, number])
        .sort((a, b) => a[0] - b[0]);
      const arranged = [...LOCS].sort((a, b) => a[0] - b[0]);
      for (let k = 0; k < arranged.length; k++) {
        for (let d = 0; d < 3; d++) {
          expect(Math.abs(locs[k]![d]! - arranged[k]![d]!) <= 1.0).toBe(true);
        }
      }
      for (const inst of got) {
        expect(Object.keys(inst.rotation).sort()).toEqual(["pitch", "roll", "yaw"]);
        expect(Object.keys(inst.scale).sort()).toEqual(["x", "y", "z"]);
      }
    });
  });

  test("foliage_inspect_instances_without_type_is_invalid_argument", async () => {
    // mode='instances' with no foliage_type must refuse, not dump everything.
    const resp: any = await ctx.mcp.call("foliage_inspect", { mode: "instances" });
    expect(resp.status).toBe("error");
    expect(resp.error_code).toBe("invalid_argument");
  });

  test("foliage_inspect_unmatched_type_is_asset_not_found", async () => {
    const resp: any = await ctx.mcp.call("foliage_inspect", {
      mode: "instances",
      foliage_type: MISSING_TYPE,
    });
    expect(resp.status).toBe("error");
    expect(resp.error_code).toBe("asset_not_found");
  });

  test("foliage_inspect_unknown_mode_is_invalid_argument", async () => {
    // C++ mode gate — reachable only over the raw wire (the MCP layer's zod
    // enum already refuses a bogus mode before it reaches the bridge).
    const resp: any = await ctx.bridge.command("foliage_inspect", { mode: "bogus" });
    expect(resp.status).toBe("error");
    expect(resp.error_code).toBe("invalid_argument");
  });
});
