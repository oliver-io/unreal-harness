"""Actor domain — spawn native actors into the transient level, mutate and
inspect them through the *legacy* actor commands, then assert via read-back.

Pairs with test_actor_level.py, which covers the newer actor_spawn / actor_query
/ level_inspect surface; this module exercises the legacy spawn_actor /
find_actors_by_name / get_actors_in_level / actor_inspect commands plus the
per-actor mutators (set_actor_transform, actor_set_property, delete_actor) and
the material-info / gamemode helpers.

Native actors live in the unsaved transient level, so editor-quit is a full
reset — no on-disk cleanup needed. Spawns are made re-runnable by deleting any
prior actor of the same name first (spawn_actor refuses a name collision).

Pattern for every test: arrange prerequisite state -> dispatch the op (raises on
a non-success envelope) -> assert the resulting state via a read/inspect op.
"""

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready

# A StaticMeshActor needs a mesh for get_actor_material_info to report a slot;
# the engine cube is always present and asset-free for the test project.
CUBE_MESH = "/Engine/BasicShapes/Cube.Cube"
NS = "/Game/__MCPTest__/actor"


def _delete_actor_if_present(bridge, name: str) -> None:
    """Arrange helper (mirrors ops.ensure_absent for assets): drop any actor of
    this name so the following spawn_actor won't hit a name collision."""
    try:
        bridge.command("actor_delete", {"name": name})
    except Exception:
        pass


def _spawn(bridge, name: str, actor_type: str = "StaticMeshActor", **extra) -> dict:
    """Idempotent spawn of a native actor; returns the spawn_actor result
    (ActorToJsonObject: name, class, location[], rotation[], scale[])."""
    _delete_actor_if_present(bridge, name)
    params = {"name": name, "type": actor_type}
    params.update(extra)
    return bridge.expect("spawn_actor", params)


@pytest.fixture(scope="module")
def sm_actor(editor_session):
    """One shared StaticMeshActor (with the engine cube) for the read-only and
    non-destructive tests. Lives in the transient level; the editor-quit reset
    cleans it up.

    Spawns at the BRIDGE level: the legacy ``spawn_actor`` command has no
    standalone MCP tool (only ``actor_spawn``), so the shared-actor setup can't
    go through the MCP client. The tests that consume this fixture inspect/mutate
    it through the real MCP tools."""
    from harness.bridge_client import BridgeClient

    bridge = (editor_session.bridge if editor_session is not None
              else BridgeClient(config.BRIDGE_HOST, config.BRIDGE_PORT))
    name = "MCPTest_SM_Shared"
    _spawn(bridge, name, static_mesh=CUBE_MESH)
    return name


@covers("find_actors_by_name", "actor_get_in_level")
def test_spawn_actor_then_find_and_list(bridge):
    """A real spawn must be addressable by name and present in the level list.

    Stays at the BRIDGE level: the legacy ``spawn_actor`` command this test
    exercises has no standalone MCP tool (only ``actor_spawn``), so the whole
    spawn->find->list sequence runs through the raw bridge."""
    name = "MCPTest_SM_Spawn"
    spawned = _spawn(bridge, name, static_mesh=CUBE_MESH)
    assert spawned["name"] == name, spawned
    assert spawned["class"].endswith("StaticMeshActor"), spawned

    found = bridge.expect("find_actors_by_name", {"pattern": name})
    names = [a.get("name") for a in found.get("actors", [])]
    assert name in names, names

    in_level = bridge.expect("actor_get_in_level")
    all_names = [a.get("name") for a in in_level.get("actors", [])]
    assert name in all_names, f"{name} not in level of {len(all_names)} actors"


@covers("actor_inspect")
def test_actor_inspect_reports_class_and_transform(mcp, sm_actor):
    result = mcp.expect("actor_inspect", {"name": sm_actor})
    assert result["name"] == sm_actor, result
    # actor_inspect reports the full class path (/Script/Engine.StaticMeshActor).
    assert "StaticMeshActor" in result["class"], result
    # Transform is an object {x,y,z}; components are enumerated.
    assert set(result["location"]) >= {"x", "y", "z"}, result
    assert "components" in result, result


@covers("actor_set_transform", "actor_inspect")
def test_set_actor_transform_then_inspect(mcp, sm_actor):
    """Move the actor, then prove the new location through a different command."""
    mcp.expect("actor_set_transform", {
        "name": sm_actor,
        "location": [120.0, 240.0, 360.0],
    })
    result = mcp.expect("actor_inspect", {"name": sm_actor})
    loc = result["location"]
    assert loc["x"] == pytest.approx(120.0, abs=1.0), loc
    assert loc["y"] == pytest.approx(240.0, abs=1.0), loc
    assert loc["z"] == pytest.approx(360.0, abs=1.0), loc


@covers("actor_set_transform")
def test_set_actor_transform_dry_run_does_not_mutate(mcp, sm_actor):
    """dry_run validates and emits a transforms_changed diff without mutating."""
    result = mcp.expect("actor_set_transform", {
        "name": sm_actor,
        "location": [1.0, 2.0, 3.0],
        "dry_run": True,
    })
    assert result.get("dry_run") is True, result
    changed = result["diff"]["transforms_changed"]
    assert changed[0]["name"] == sm_actor, changed


@covers("actor_set_property")
def test_actor_set_property_then_readback(mcp, sm_actor):
    """Reflective write of a leaf on a placed actor; the response re-exports the
    property after the write, which is the read-back."""
    result = mcp.expect("actor_set_property", {
        "name": sm_actor,
        "property": "StaticMeshComponent.BoundsScale",
        "value": 2.0,
    })
    assert result.get("success") is True, result
    # Default BoundsScale is 1.0; the exported 'after' text must reflect 2.0.
    assert "2" in result["after"], result
    assert result["before"] != result["after"], result


@covers("actor_delete", "find_actors_by_name")
def test_delete_actor_then_absent(bridge, mcp):
    """Delete a dedicated actor (not the shared one) and prove it's gone.

    The setup spawn uses the legacy ``spawn_actor`` command (no MCP tool) via the
    bridge; the tools under test — delete_actor and find_actors_by_name — run
    through the real MCP server."""
    name = "MCPTest_SM_ToDelete"
    _spawn(bridge, name, static_mesh=CUBE_MESH)

    result = mcp.expect("actor_delete", {"name": name})
    assert "deleted_actor" in result, result

    found = mcp.expect("find_actors_by_name", {"pattern": name})
    names = [a.get("name") for a in found.get("actors", [])]
    assert name not in names, f"{name} still present after delete: {names}"


@covers("mesh_get_actor_material_info")
def test_get_actor_material_info(mcp, sm_actor):
    """The shared actor carries the engine cube, so it must report >=1 slot."""
    result = mcp.expect("mesh_get_actor_material_info", {"actor_name": sm_actor})
    assert result["actor_name"] == sm_actor, result
    assert result["total_slots"] >= 1, result
    slots = result["material_slots"]
    assert slots and "material_path" in slots[0], result


@covers("mesh_set_mesh_material_color")
def test_set_mesh_material_color(mcp):
    """GAP-009: mesh_set_mesh_material_color targets a Blueprint SCS component
    *template* and now REFUSES the dynamic-instance path (a runtime MID baked into
    a saved template corrupts level saves). It must return a structured
    feature_disabled error directing to the saved-asset path."""
    bp = f"{NS}/BP_MeshColor"
    ensure_absent(mcp, bp)
    mcp.expect("bp_create_blueprint", {"name": bp, "parent_class": "Actor"})
    mcp.expect("bp_add_component", {
        "blueprint_name": bp,
        "component_type": "StaticMeshComponent",
        "component_name": "Mesh",
    })
    mcp.expect("bp_compile", {"blueprint_name": bp})

    # Use call() (not expect()) — this is an INTENTIONAL refusal, not a failure.
    result = mcp.call("mesh_set_mesh_material_color", {
        "blueprint_name": bp,
        "component_name": "Mesh",
        "color": [1.0, 0.25, 0.0, 1.0],
        "material_path": "/Engine/BasicShapes/BasicShapeMaterial",
        "parameter_name": "BaseColor",
    })
    assert result.get("status") == "error", result
    assert result.get("error_code") == "feature_disabled", result
    # The hint must point at the saved-asset replacement path.
    assert "material_create_instance" in str(result), result


@covers("level_set_gamemode_override")
def test_set_world_gamemode_override(mcp):
    """Bind a level's World Settings GameModeOverride. Requires a saved World
    asset to target — the transient untitled level has no /Game path, so we
    discover an on-disk level and skip cleanly if the project has none."""
    listing = mcp.expect("asset_list", {
        "directory_path": "/Game/",
        "recursive": True,
        "class_filter": "World",
    })
    data = listing.get("data", listing)
    worlds = [a for a in data.get("assets", []) if a.get("class") == "World"]
    if not worlds:
        pytest.skip("no saved World/level asset in the test project to target")

    level_path = worlds[0]["path"].split(".")[0]
    result = mcp.expect("level_set_gamemode_override", {
        "level_path": level_path,
        "gamemode_class": "/Script/Engine.GameModeBase",
    })
    assert result.get("success") is True, result
    assert "GameMode" in result.get("gamemode_class", ""), result
    assert_ready(mcp)
