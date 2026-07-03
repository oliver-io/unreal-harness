"""PCG domain — discovery, graph authoring, and driving a component to
generate (``MCPPCGCommands.cpp``). Parity twin:
``src/server/test/integration/pcg.test.ts``.

The PCG module is a hard dependency of the UnrealMCP plugin (``UnrealMCP.uplugin``
/ ``UnrealMCP.Build.cs`` list "PCG"), so these ops are available in any editor
the plugin loads in — no plugin-enabled gating needed.

Arrange strategy — everything is authorable from scratch through the typed PCG
surface, so no imported content is required and nothing is gui_only:

- Asset tier: graphs live under ``/Game/__MCPTest__/pcg/``; every create is
  preceded by ``ensure_absent`` and each test deletes its own asset, so the
  suite re-runs cleanly against a long-lived shared editor. Topology written by
  ``pcg_node_add``/``pcg_node_connect`` is observed through ``pcg_graph_read``
  (a different, read-only primitive), asset existence through
  ``pcg_list_graphs`` (asset-registry backed).

- World tier (component add / generate): the observable is a DETERMINISTIC,
  world-independent graph — CreatePointsGrid (GridExtents 100x100x50, cell 100
  => exactly a 2x2x1 grid of points, no landscape/surface dependency) feeding a
  SpawnActor node (TemplateActorClass=PointLight, Option=NoMerging), generated
  on a spawned host actor in a far corner with CoordinateSpace=LocalComponent so
  the 4 spawned lights land at host±50 — far from anything real in a shared
  attached level. The spawn locations double as the behavioural proof that
  ``pcg_node_set_property`` writes stuck (extents shrank the grid; the
  coordinate space recentred it): there is no settings-reader primitive, so the
  property write is observed through what generation actually produced.

Self-cleaning (validated live 2026-07-03): a force re-generate cleans up the
previous generation's actors, but DELETING THE HOST DOES NOT delete generated
actors — they are separate level actors in an ``<Host>_Generated`` outliner
folder. Teardown therefore deletes the generated lights explicitly (by the
baseline-vs-after name delta) before deleting the host and the graph. PCG
generation is asynchronous (scheduled on the PCG subsystem), so the test polls
``actor_query`` to a deadline — never a blind sleep.
"""

import time

import pytest

from harness.coverage import covers
from harness.ops import ensure_absent

NS = "/Game/__MCPTest__/pcg"

GRID_CLASS = "PCGCreatePointsGridSettings"
SPAWNER_CLASS = "PCGSpawnActorSettings"
POINT_LIGHT = "/Script/Engine.PointLight"

HOST = "MCPTest_PCG_GenHost"
HOST_LOC = {"x": 90000.0, "y": 90000.0, "z": 0.0}
# GridExtents are half-sizes: 100x100 with the default 100 cell => 2x2 cells;
# Z extent 50 => a single layer. CoordinateSpace=LocalComponent centres the
# grid on the host, so cell centres land at host ± 50 on X and Y.
GRID_EXTENTS = {"x": 100.0, "y": 100.0, "z": 50.0}
EXPECTED_SPAWNS = 4
GENERATE_DEADLINE_S = 30.0


def _light_names(mcp) -> set:
    result = mcp.expect("actor_query", {"class_filter": POINT_LIGHT, "limit": 1000})
    return {a["name"] for a in result.get("actors") or []}


@pytest.fixture
def pcg_graph(mcp, request):
    """One empty PCG graph per test, deleted afterward (attach-mode friendly —
    the session-end disk wipe only covers the launch-mode fixture project)."""
    safe = "".join(c if (c.isalnum() or c == "_") else "_" for c in request.node.name)
    path = f"{NS}/PCG_{safe[:48]}"
    ensure_absent(mcp, path)
    mcp.expect("pcg_graph_create", {"graph_path": path})
    try:
        yield path
    finally:
        ensure_absent(mcp, path)


# ── discovery ────────────────────────────────────────────────────────────────

@covers("pcg_list_node_types")
def test_list_node_types_reports_create_points_grid(mcp):
    """The palette must expose the engine's CreatePointsGrid settings class
    with its class_path, category type, and declared output pin."""
    result = mcp.expect("pcg_list_node_types", {"name_filter": "CreatePoints"})
    types = result.get("node_types") or []
    assert result.get("count") == len(types) and types, result
    grid = next((t for t in types if t.get("class_name") == GRID_CLASS), None)
    assert grid, [t.get("class_name") for t in types]
    assert grid["class_path"] == f"/Script/PCG.{GRID_CLASS}", grid
    assert grid["type"] == "Spatial", grid
    assert "Out" in (grid.get("output_pins") or []), grid


@covers("pcg_list_node_types")
def test_list_node_types_category_filter_returns_only_spawners(mcp):
    result = mcp.expect("pcg_list_node_types", {"category_filter": "Spawner"})
    types = result.get("node_types") or []
    assert types, result
    assert all(t.get("type") == "Spawner" for t in types), types
    assert SPAWNER_CLASS in [t.get("class_name") for t in types], types


# ── graph lifecycle ──────────────────────────────────────────────────────────

@covers("pcg_graph_create", "pcg_list_graphs", "pcg_graph_read")
def test_graph_create_then_registry_listing_and_read(mcp):
    """Create an empty graph, then prove it exists via the asset registry
    (pcg_list_graphs) and has exactly its Input/Output nodes and no edges
    (pcg_graph_read) — both different, read-only primitives."""
    path = f"{NS}/PCG_Created"
    ensure_absent(mcp, path)
    try:
        created = mcp.expect("pcg_graph_create", {"graph_path": path})
        assert created.get("asset_path") == path, created
        assert created.get("input_node") and created.get("output_node"), created

        listing = mcp.expect("pcg_list_graphs", {"path_filter": NS})
        entries = listing.get("graphs") or []
        match = next((g for g in entries if g.get("path", "").startswith(path)), None)
        assert match, entries
        assert match["class"] == "PCGGraph", match
        assert match["name"] == "PCG_Created", match

        read = mcp.expect("pcg_graph_read", {"graph_path": path})
        assert read.get("input_node") == created["input_node"], read
        assert read.get("output_node") == created["output_node"], read
        assert read.get("node_count") == 2, read  # just Input + Output
        assert read.get("edges") == [], read
    finally:
        ensure_absent(mcp, path)


@covers("pcg_graph_create")
def test_graph_create_existing_path_is_name_collision(mcp, pcg_graph):
    resp = mcp.call("pcg_graph_create", {"graph_path": pcg_graph})
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "name_collision", resp


@covers("pcg_graph_read")
def test_graph_read_missing_graph_is_asset_not_found(mcp):
    resp = mcp.call("pcg_graph_read", {"graph_path": f"{NS}/PCG_NoSuchGraph"})
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "asset_not_found", resp


# ── node authoring, observed via pcg_graph_read ──────────────────────────────

@covers("pcg_node_add", "pcg_node_connect", "pcg_graph_read")
def test_node_add_and_connect_observed_via_graph_read(mcp, pcg_graph):
    """Author Grid -> Spawner -> Output and prove the full topology (nodes with
    settings_class + titles, both edges with resolved pins) through the reader."""
    grid = mcp.expect("pcg_node_add", {
        "graph_path": pcg_graph, "settings_class": GRID_CLASS, "node_title": "Grid",
    })["node"]["id"]
    spawner = mcp.expect("pcg_node_add", {
        "graph_path": pcg_graph, "settings_class": SPAWNER_CLASS, "node_title": "Spawner",
    })["node"]["id"]

    # Titles double as node handles; connect defaults resolve the pins.
    first = mcp.expect("pcg_node_connect", {
        "graph_path": pcg_graph, "from_node": "Grid", "to_node": "Spawner", "to_pin": "In",
    })
    assert first.get("from_pin") == "Out", first  # defaulted to first output pin
    mcp.expect("pcg_node_connect", {
        "graph_path": pcg_graph, "from_node": "Spawner", "to_node": "OutputNode",
    })

    read = mcp.expect("pcg_graph_read", {"graph_path": pcg_graph})
    by_id = {n["id"]: n for n in read.get("nodes") or []}
    assert by_id.get(grid, {}).get("settings_class") == GRID_CLASS, by_id
    assert by_id.get(grid, {}).get("title") == "Grid", by_id
    assert by_id.get(spawner, {}).get("settings_class") == SPAWNER_CLASS, by_id
    assert read.get("node_count") == 4, read  # In + Out + Grid + Spawner

    edges = {(e["from_node"], e["from_pin"], e["to_node"], e["to_pin"])
             for e in read.get("edges") or []}
    assert (grid, "Out", spawner, "In") in edges, edges
    assert (spawner, "Out", read["output_node"], "Out") in edges, edges


@covers("pcg_node_add")
def test_node_add_unknown_settings_class_is_class_not_loaded(mcp, pcg_graph):
    resp = mcp.call("pcg_node_add", {
        "graph_path": pcg_graph, "settings_class": "PCGNoSuchSettings",
    })
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "class_not_loaded", resp


@covers("pcg_node_connect")
def test_node_connect_unknown_node_is_node_not_found(mcp, pcg_graph):
    resp = mcp.call("pcg_node_connect", {
        "graph_path": pcg_graph, "from_node": "NoSuchNode", "to_node": "OutputNode",
    })
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "node_not_found", resp


@covers("pcg_node_set_property")
def test_node_set_property_unknown_property_is_invalid_argument(mcp, pcg_graph):
    mcp.expect("pcg_node_add", {
        "graph_path": pcg_graph, "settings_class": GRID_CLASS, "node_title": "Grid",
    })
    resp = mcp.call("pcg_node_set_property", {
        "graph_path": pcg_graph, "node": "Grid",
        "property_name": "NoSuchProperty", "property_value": 1,
    })
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "invalid_argument", resp


# ── component add + generate, end to end ─────────────────────────────────────

@covers("pcg_component_add", "pcg_component_generate", "pcg_node_set_property")
def test_component_add_and_generate_spawns_deterministic_grid(mcp, pcg_graph):
    """The full drive path. Arrange a world-independent generator (points grid
    -> spawn PointLight actors) on a spawned host; act via pcg_component_add +
    pcg_component_generate; observe through actor_inspect (the component
    arrived) and actor_query (exactly 4 new PointLights at host±50, PCG-tagged).
    The ±50 local-space locations are also the observable proof of the
    pcg_node_set_property writes (GridExtents / CoordinateSpace / template
    class / spawn option), since no settings-reader primitive exists."""
    # Graph: Grid -> Spawner -> Output, fully deterministic, no world deps.
    mcp.expect("pcg_node_add", {
        "graph_path": pcg_graph, "settings_class": GRID_CLASS, "node_title": "Grid",
    })
    mcp.expect("pcg_node_add", {
        "graph_path": pcg_graph, "settings_class": SPAWNER_CLASS, "node_title": "Spawner",
    })
    for node, prop, value in (
        ("Grid", "GridExtents", GRID_EXTENTS),
        ("Grid", "CoordinateSpace", "LocalComponent"),
        ("Spawner", "TemplateActorClass", POINT_LIGHT),
        ("Spawner", "Option", "NoMerging"),
    ):
        mcp.expect("pcg_node_set_property", {
            "graph_path": pcg_graph, "node": node,
            "property_name": prop, "property_value": value,
        })
    mcp.expect("pcg_node_connect", {
        "graph_path": pcg_graph, "from_node": "Grid", "to_node": "Spawner", "to_pin": "In",
    })
    mcp.expect("pcg_node_connect", {
        "graph_path": pcg_graph, "from_node": "Spawner", "to_node": "OutputNode",
    })

    # Host actor in a far corner of the (possibly shared) level.
    try:
        mcp.call("actor_delete", {"name": HOST})  # idempotent re-runs
    except Exception:
        pass
    spawned = mcp.expect("actor_spawn", {
        "class_path": "/Script/Engine.StaticMeshActor",
        "name": HOST, "location": HOST_LOC,
    })
    host = spawned["actor"]["name"]

    new_lights: set = set()
    try:
        # Negative gate first: generate before any component exists.
        resp = mcp.call("pcg_component_generate", {"actor_name": host})
        assert resp.get("status") == "error", resp
        assert resp.get("error_code") == "actor_not_found", resp

        added = mcp.expect("pcg_component_add", {
            "actor_name": host, "graph_path": pcg_graph,
        })
        component = added.get("component")
        assert component, added

        # Independent observation: the component is live on the actor.
        inspected = mcp.expect("actor_inspect", {"name": host})
        comps = {c["name"]: c["class"] for c in inspected.get("components") or []}
        assert comps.get(component) == "/Script/PCG.PCGComponent", comps

        baseline = _light_names(mcp)
        gen = mcp.expect("pcg_component_generate", {"actor_name": host})
        assert gen.get("generation_requested") is True, gen

        # Async generation: poll the level to a deadline, never a blind sleep.
        deadline = time.time() + GENERATE_DEADLINE_S
        while time.time() < deadline:
            new_lights = _light_names(mcp) - baseline
            if len(new_lights) >= EXPECTED_SPAWNS:
                break
            time.sleep(0.25)
        assert len(new_lights) == EXPECTED_SPAWNS, (
            f"expected exactly {EXPECTED_SPAWNS} PCG-spawned PointLights, "
            f"got {sorted(new_lights)}"
        )

        # Deterministic 2x2 local-space grid: every light at host ± 50 on X/Y,
        # PCG-tagged — and all four cells distinct.
        result = mcp.expect("actor_query", {"class_filter": POINT_LIGHT, "limit": 1000})
        entries = [a for a in result.get("actors") or [] if a["name"] in new_lights]
        assert len(entries) == EXPECTED_SPAWNS, entries
        cells = set()
        for entry in entries:
            assert "PCG Generated Actor" in (entry.get("tags") or []), entry
            dx = entry["location"]["x"] - HOST_LOC["x"]
            dy = entry["location"]["y"] - HOST_LOC["y"]
            assert abs(abs(dx) - 50.0) <= 1.0 and abs(abs(dy) - 50.0) <= 1.0, entry
            cells.add((round(dx), round(dy)))
        assert len(cells) == EXPECTED_SPAWNS, cells
    finally:
        # Generated actors are NOT children of the host — delete them explicitly.
        for name in new_lights:
            try:
                mcp.call("actor_delete", {"name": name})
            except Exception:
                pass
        try:
            mcp.call("actor_delete", {"name": host})
        except Exception:
            pass


@covers("pcg_component_add")
def test_component_add_unknown_actor_is_actor_not_found(mcp):
    resp = mcp.call("pcg_component_add", {"actor_name": "MCPTest_NoSuchPCGHost"})
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "actor_not_found", resp


@covers("pcg_component_generate")
def test_component_generate_unknown_actor_is_actor_not_found(mcp):
    resp = mcp.call("pcg_component_generate", {"actor_name": "MCPTest_NoSuchPCGHost"})
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "actor_not_found", resp
