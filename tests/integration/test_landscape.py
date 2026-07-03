"""Landscape read-only ops: landscape_inspect / landscape_list_layers /
landscape_read_heightmap (``MCPLandscapeCommands.cpp``).

Arrange strategy — a BARE ``ALandscape`` spawned through the typed
``actor_spawn`` primitive (``/Script/Landscape.Landscape``). A bare proxy has
no landscape components, which is exactly what the harness can arrange:
neither the typed surface nor the UE 5.7 Python API can create landscape
COMPONENTS (``unreal.LandscapeProxy`` only exposes
``landscape_import_heightmap_from_render_target``, which requires components
to already exist). That still yields real, observable contracts:

- ``landscape_inspect``: full positive path — enumeration + find-by-name of a
  live ``ALandscapeProxy`` (class label, transform, quad config).
- ``landscape_list_layers``: success path — a component-less landscape has no
  ``ULandscapeInfo`` and therefore an empty paint-layer set.
- ``landscape_read_heightmap``: the two documented error gates — unknown
  actor (``actor_not_found``) and no-components (``invalid_argument``).

DEFERRED (see docs/loops/tests/TASKS.md): the value-bearing heightmap /
assigned-paint-layer positive paths (known raw heights, height_stats, layer
assignments) need a component-bearing landscape, which no arrange primitive
can build today.

Attach-safe: the arrange is one actor spawned into (and always deleted from)
the open level; nothing is saved. A 2026-07-02 live probe confirmed
spawn -> observe -> delete leaves zero residue (landscape_inspect count and
actor_query both return to baseline).
"""

import pytest

from harness.coverage import covers

LANDSCAPE_CLASS = "/Script/Landscape.Landscape"
ACTOR = "MCPTest_Landscape"
MISSING = "MCPTest_NoSuchLandscape"
# Far corner: irrelevant to a component-less landscape, but keeps the marker
# actor away from anything real in a shared attached level.
SPAWN_LOC = {"x": 100000.0, "y": 100000.0, "z": -10000.0}


@pytest.fixture
def landscape_actor(mcp):
    """One bare ALandscape, always deleted afterward (attach-safe teardown).

    Yields ``(fname, label)``: after a same-name delete the FName stays
    reserved until GC, so a respawn can come back suffixed (``_0``) while the
    editor LABEL stays clean — handlers match either but echo the label."""
    try:
        mcp.call("actor_delete", {"name": ACTOR})  # idempotent re-runs
    except Exception:
        pass
    spawned = mcp.expect("actor_spawn", {
        "class_path": LANDSCAPE_CLASS,
        "name": ACTOR,
        "location": SPAWN_LOC,
    })
    name = spawned["actor"]["name"]
    label = spawned["actor"]["label"]
    try:
        yield name, label
    finally:
        try:
            mcp.call("actor_delete", {"name": name})
        except Exception:
            pass


@covers("landscape_inspect")
def test_landscape_inspect_reports_spawned_landscape(mcp, landscape_actor):
    """Find-by-name must return exactly the spawned proxy with its class label
    and the transform the (different) arrange primitive gave it."""
    name, label = landscape_actor
    result = mcp.expect("landscape_inspect", {"actor_name": name})
    assert result["count"] == 1, result
    entry = result["landscapes"][0]
    assert entry["name"] == label            # handler echoes the actor label
    assert entry["internal_name"] == name
    assert entry["class"] == "Landscape"
    assert abs(entry["location"]["x"] - SPAWN_LOC["x"]) <= 1.0
    assert abs(entry["location"]["z"] - SPAWN_LOC["z"]) <= 1.0
    # A bare proxy has no components: quad config is zeroed, no extent block.
    assert entry["component_size_quads"] == 0
    assert "extent_quads" not in entry

    # Enumeration (no actor_name) must include it. >= tolerates a shared
    # attached level that genuinely contains other landscapes.
    listing = mcp.expect("landscape_inspect")
    assert listing["count"] >= 1
    assert label in [e["name"] for e in listing["landscapes"]]


@covers("landscape_inspect")
def test_landscape_inspect_unknown_name_is_actor_not_found(mcp):
    """A non-empty actor_name never falls back to 'first landscape' — a bogus
    name must fail actor_not_found regardless of level content."""
    resp = mcp.call("landscape_inspect", {"actor_name": MISSING})
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "actor_not_found", resp


@covers("landscape_list_layers")
def test_landscape_list_layers_component_less_landscape_is_empty(mcp, landscape_actor):
    """Success path: the op resolves the proxy and reports its paint-layer set
    — empty for a component-less landscape (no ULandscapeInfo layers)."""
    name, label = landscape_actor
    result = mcp.expect("landscape_list_layers", {"actor_name": name})
    assert result["actor"] == label          # handler echoes the actor label
    assert result["count"] == 0, result
    assert result["layers"] == []


@covers("landscape_list_layers")
def test_landscape_list_layers_unknown_name_is_actor_not_found(mcp):
    resp = mcp.call("landscape_list_layers", {"actor_name": MISSING})
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "actor_not_found", resp


@covers("landscape_read_heightmap")
def test_landscape_read_heightmap_component_less_landscape_is_invalid_argument(
        mcp, landscape_actor):
    """The no-ULandscapeInfo gate: a resolved landscape without loaded
    components must refuse with invalid_argument (documented contract), not
    crash or return an empty grid."""
    name, _label = landscape_actor
    resp = mcp.call("landscape_read_heightmap", {"actor_name": name})
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "invalid_argument", resp
    assert "ULandscapeInfo" in str(resp.get("error", "")), resp


@covers("landscape_read_heightmap")
def test_landscape_read_heightmap_unknown_name_is_actor_not_found(mcp):
    resp = mcp.call("landscape_read_heightmap", {"actor_name": MISSING})
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "actor_not_found", resp
