/**
 * PCG domain — discovery, graph authoring, and driving a component to
 * generate. Parity twin of tests/integration/test_pcg.py (the pytest module
 * carries the @covers ledger — these are bridge ops).
 *
 * The PCG module is a hard dependency of the UnrealMCP plugin, so the ops are
 * available in any editor the plugin loads in — no gating needed, and every
 * test here is shared-editor safe (namespaced assets under /Game/__MCPTest__/
 * pcg/, far-corner actors, explicit teardown, no level saves).
 *
 * World-tier observable (validated live 2026-07-03): a deterministic,
 * world-independent graph — CreatePointsGrid (extents 100x100x50, cell 100 =>
 * exactly 2x2x1 points) -> SpawnActor (PointLight, NoMerging) — generated with
 * CoordinateSpace=LocalComponent on a far-corner host yields exactly 4
 * PCG-tagged PointLights at host±50. Generated actors are NOT children of the
 * host (they live in an <Host>_Generated outliner folder), so teardown deletes
 * them explicitly by name delta. Generation is async: poll to a deadline.
 */

import { test, expect } from "bun:test";
import { editorSuite, type Ctx } from "../harness/suite.ts";

const NS = "/Game/__MCPTest__/pcg";

const GRID_CLASS = "PCGCreatePointsGridSettings";
const SPAWNER_CLASS = "PCGSpawnActorSettings";
const POINT_LIGHT = "/Script/Engine.PointLight";

const HOST = "MCPTest_PCG_GenHostBun";
const HOST_LOC = { x: 90000.0, y: 90000.0, z: 0.0 };
// Half-size extents: 100x100 at the default 100 cell => 2x2 cells; Z 50 => one
// layer. LocalComponent space centres the grid on the host => cells at ±50.
const GRID_EXTENTS = { x: 100.0, y: 100.0, z: 50.0 };
const EXPECTED_SPAWNS = 4;
const GENERATE_DEADLINE_MS = 30_000;

async function ensureAssetAbsent(ctx: Ctx, assetPath: string): Promise<void> {
  try {
    await ctx.mcp.call("asset_delete", { asset_path: assetPath, force: true });
  } catch {
    /* absent is fine */
  }
}

/** One empty PCG graph, always deleted afterward. */
async function withGraph(
  ctx: Ctx,
  name: string,
  body: (graphPath: string) => Promise<void>,
): Promise<void> {
  const path = `${NS}/${name}`;
  await ensureAssetAbsent(ctx, path);
  await ctx.mcp.expect("pcg_graph_create", { graph_path: path });
  try {
    await body(path);
  } finally {
    await ensureAssetAbsent(ctx, path);
  }
}

async function lightNames(ctx: Ctx): Promise<Set<string>> {
  const result: any = await ctx.mcp.expect("actor_query", {
    class_filter: POINT_LIGHT,
    limit: 1000,
  });
  return new Set((result.actors ?? []).map((a: any) => String(a.name)));
}

editorSuite("pcg", (ctx) => {
  // ── discovery ──────────────────────────────────────────────────────────

  test("pcg_list_node_types_reports_create_points_grid", async () => {
    const result: any = await ctx.mcp.expect("pcg_list_node_types", {
      name_filter: "CreatePoints",
    });
    const types: any[] = result.node_types ?? [];
    expect(result.count).toBe(types.length);
    const grid = types.find((t) => t.class_name === GRID_CLASS);
    expect(grid).toBeDefined();
    expect(grid.class_path).toBe(`/Script/PCG.${GRID_CLASS}`);
    expect(grid.type).toBe("Spatial");
    expect(grid.output_pins).toContain("Out");
  });

  test("pcg_list_node_types_category_filter_returns_only_spawners", async () => {
    const result: any = await ctx.mcp.expect("pcg_list_node_types", {
      category_filter: "Spawner",
    });
    const types: any[] = result.node_types ?? [];
    expect(types.length).toBeGreaterThan(0);
    expect(types.every((t) => t.type === "Spawner")).toBe(true);
    expect(types.map((t) => t.class_name)).toContain(SPAWNER_CLASS);
  });

  // ── graph lifecycle ────────────────────────────────────────────────────

  test("pcg_graph_create_then_registry_listing_and_read", async () => {
    const path = `${NS}/PCG_CreatedBun`;
    await ensureAssetAbsent(ctx, path);
    try {
      const created: any = await ctx.mcp.expect("pcg_graph_create", { graph_path: path });
      expect(created.asset_path).toBe(path);
      expect(created.input_node).toBeTruthy();
      expect(created.output_node).toBeTruthy();

      const listing: any = await ctx.mcp.expect("pcg_list_graphs", { path_filter: NS });
      const match = (listing.graphs ?? []).find((g: any) => String(g.path).startsWith(path));
      expect(match).toBeDefined();
      expect(match.class).toBe("PCGGraph");
      expect(match.name).toBe("PCG_CreatedBun");

      const read: any = await ctx.mcp.expect("pcg_graph_read", { graph_path: path });
      expect(read.input_node).toBe(created.input_node);
      expect(read.output_node).toBe(created.output_node);
      expect(read.node_count).toBe(2); // just Input + Output
      expect(read.edges).toEqual([]);
    } finally {
      await ensureAssetAbsent(ctx, path);
    }
  });

  test("pcg_graph_create_existing_path_is_name_collision", async () => {
    await withGraph(ctx, "PCG_CollideBun", async (path) => {
      const resp: any = await ctx.mcp.call("pcg_graph_create", { graph_path: path });
      expect(resp.status).toBe("error");
      expect(resp.error_code).toBe("name_collision");
    });
  });

  test("pcg_graph_read_missing_graph_is_asset_not_found", async () => {
    const resp: any = await ctx.mcp.call("pcg_graph_read", {
      graph_path: `${NS}/PCG_NoSuchGraph`,
    });
    expect(resp.status).toBe("error");
    expect(resp.error_code).toBe("asset_not_found");
  });

  // ── node authoring, observed via pcg_graph_read ────────────────────────

  test("pcg_node_add_and_connect_observed_via_graph_read", async () => {
    await withGraph(ctx, "PCG_TopologyBun", async (path) => {
      const grid = (
        (await ctx.mcp.expect("pcg_node_add", {
          graph_path: path,
          settings_class: GRID_CLASS,
          node_title: "Grid",
        })) as any
      ).node.id as string;
      const spawner = (
        (await ctx.mcp.expect("pcg_node_add", {
          graph_path: path,
          settings_class: SPAWNER_CLASS,
          node_title: "Spawner",
        })) as any
      ).node.id as string;

      // Titles double as node handles; connect defaults resolve the pins.
      const first: any = await ctx.mcp.expect("pcg_node_connect", {
        graph_path: path,
        from_node: "Grid",
        to_node: "Spawner",
        to_pin: "In",
      });
      expect(first.from_pin).toBe("Out"); // defaulted to first output pin
      await ctx.mcp.expect("pcg_node_connect", {
        graph_path: path,
        from_node: "Spawner",
        to_node: "OutputNode",
      });

      const read: any = await ctx.mcp.expect("pcg_graph_read", { graph_path: path });
      const byId = new Map((read.nodes ?? []).map((n: any) => [n.id, n]));
      expect((byId.get(grid) as any)?.settings_class).toBe(GRID_CLASS);
      expect((byId.get(grid) as any)?.title).toBe("Grid");
      expect((byId.get(spawner) as any)?.settings_class).toBe(SPAWNER_CLASS);
      expect(read.node_count).toBe(4); // In + Out + Grid + Spawner

      const edges = new Set(
        (read.edges ?? []).map(
          (e: any) => `${e.from_node}.${e.from_pin}->${e.to_node}.${e.to_pin}`,
        ),
      );
      expect(edges.has(`${grid}.Out->${spawner}.In`)).toBe(true);
      expect(edges.has(`${spawner}.Out->${read.output_node}.Out`)).toBe(true);
    });
  });

  test("pcg_node_add_unknown_settings_class_is_class_not_loaded", async () => {
    await withGraph(ctx, "PCG_BadClassBun", async (path) => {
      const resp: any = await ctx.mcp.call("pcg_node_add", {
        graph_path: path,
        settings_class: "PCGNoSuchSettings",
      });
      expect(resp.status).toBe("error");
      expect(resp.error_code).toBe("class_not_loaded");
    });
  });

  test("pcg_node_connect_unknown_node_is_node_not_found", async () => {
    await withGraph(ctx, "PCG_BadNodeBun", async (path) => {
      const resp: any = await ctx.mcp.call("pcg_node_connect", {
        graph_path: path,
        from_node: "NoSuchNode",
        to_node: "OutputNode",
      });
      expect(resp.status).toBe("error");
      expect(resp.error_code).toBe("node_not_found");
    });
  });

  test("pcg_node_set_property_unknown_property_is_invalid_argument", async () => {
    await withGraph(ctx, "PCG_BadPropBun", async (path) => {
      await ctx.mcp.expect("pcg_node_add", {
        graph_path: path,
        settings_class: GRID_CLASS,
        node_title: "Grid",
      });
      const resp: any = await ctx.mcp.call("pcg_node_set_property", {
        graph_path: path,
        node: "Grid",
        property_name: "NoSuchProperty",
        property_value: 1,
      });
      expect(resp.status).toBe("error");
      expect(resp.error_code).toBe("invalid_argument");
    });
  });

  // ── component add + generate, end to end ───────────────────────────────

  test(
    "pcg_component_add_and_generate_spawns_deterministic_grid",
    async () => {
      await withGraph(ctx, "PCG_GenBun", async (path) => {
        // Graph: Grid -> Spawner -> Output, fully deterministic, no world deps.
        await ctx.mcp.expect("pcg_node_add", {
          graph_path: path,
          settings_class: GRID_CLASS,
          node_title: "Grid",
        });
        await ctx.mcp.expect("pcg_node_add", {
          graph_path: path,
          settings_class: SPAWNER_CLASS,
          node_title: "Spawner",
        });
        const props: Array<[string, string, unknown]> = [
          ["Grid", "GridExtents", GRID_EXTENTS],
          ["Grid", "CoordinateSpace", "LocalComponent"],
          ["Spawner", "TemplateActorClass", POINT_LIGHT],
          ["Spawner", "Option", "NoMerging"],
        ];
        for (const [node, property_name, property_value] of props) {
          await ctx.mcp.expect("pcg_node_set_property", {
            graph_path: path,
            node,
            property_name,
            property_value,
          });
        }
        await ctx.mcp.expect("pcg_node_connect", {
          graph_path: path,
          from_node: "Grid",
          to_node: "Spawner",
          to_pin: "In",
        });
        await ctx.mcp.expect("pcg_node_connect", {
          graph_path: path,
          from_node: "Spawner",
          to_node: "OutputNode",
        });

        // Host actor in a far corner of the (possibly shared) level.
        try {
          await ctx.mcp.call("actor_delete", { name: HOST }); // idempotent
        } catch {
          /* absent is fine */
        }
        const spawned: any = await ctx.mcp.expect("actor_spawn", {
          class_path: "/Script/Engine.StaticMeshActor",
          name: HOST,
          location: HOST_LOC,
        });
        const host = String(spawned.actor.name);

        let newLights = new Set<string>();
        try {
          // Negative gate first: generate before any component exists.
          const noComp: any = await ctx.mcp.call("pcg_component_generate", {
            actor_name: host,
          });
          expect(noComp.status).toBe("error");
          expect(noComp.error_code).toBe("actor_not_found");

          const added: any = await ctx.mcp.expect("pcg_component_add", {
            actor_name: host,
            graph_path: path,
          });
          const component = String(added.component);
          expect(component.length).toBeGreaterThan(0);

          // Independent observation: the component is live on the actor.
          const inspected: any = await ctx.mcp.expect("actor_inspect", { name: host });
          const comp = (inspected.components ?? []).find((c: any) => c.name === component);
          expect(comp?.class).toBe("/Script/PCG.PCGComponent");

          const baseline = await lightNames(ctx);
          const gen: any = await ctx.mcp.expect("pcg_component_generate", {
            actor_name: host,
          });
          expect(gen.generation_requested).toBe(true);

          // Async generation: poll the level to a deadline, never a blind sleep.
          const deadline = Date.now() + GENERATE_DEADLINE_MS;
          while (Date.now() < deadline) {
            const now = await lightNames(ctx);
            newLights = new Set([...now].filter((n) => !baseline.has(n)));
            if (newLights.size >= EXPECTED_SPAWNS) break;
            await new Promise((r) => setTimeout(r, 250));
          }
          expect([...newLights].sort().length).toBe(EXPECTED_SPAWNS);

          // Deterministic 2x2 local-space grid: every light at host ± 50 on
          // X/Y, PCG-tagged — and all four cells distinct.
          const result: any = await ctx.mcp.expect("actor_query", {
            class_filter: POINT_LIGHT,
            limit: 1000,
          });
          const entries = (result.actors ?? []).filter((a: any) => newLights.has(a.name));
          expect(entries.length).toBe(EXPECTED_SPAWNS);
          const cells = new Set<string>();
          for (const entry of entries) {
            expect(entry.tags ?? []).toContain("PCG Generated Actor");
            const dx = entry.location.x - HOST_LOC.x;
            const dy = entry.location.y - HOST_LOC.y;
            expect(Math.abs(Math.abs(dx) - 50.0)).toBeLessThanOrEqual(1.0);
            expect(Math.abs(Math.abs(dy) - 50.0)).toBeLessThanOrEqual(1.0);
            cells.add(`${Math.round(dx)},${Math.round(dy)}`);
          }
          expect(cells.size).toBe(EXPECTED_SPAWNS);
        } finally {
          // Generated actors are NOT children of the host — delete explicitly.
          for (const name of newLights) {
            try {
              await ctx.mcp.call("actor_delete", { name });
            } catch {
              /* best effort */
            }
          }
          try {
            await ctx.mcp.call("actor_delete", { name: host });
          } catch {
            /* best effort */
          }
        }
      });
    },
    45_000,
  );

  test("pcg_component_add_unknown_actor_is_actor_not_found", async () => {
    const resp: any = await ctx.mcp.call("pcg_component_add", {
      actor_name: "MCPTest_NoSuchPCGHost",
    });
    expect(resp.status).toBe("error");
    expect(resp.error_code).toBe("actor_not_found");
  });

  test("pcg_component_generate_unknown_actor_is_actor_not_found", async () => {
    const resp: any = await ctx.mcp.call("pcg_component_generate", {
      actor_name: "MCPTest_NoSuchPCGHost",
    });
    expect(resp.status).toBe("error");
    expect(resp.error_code).toBe("actor_not_found");
  });
});
