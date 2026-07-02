"""The canonical mutate -> inspect -> assert loop.

Spawns a native PointLight (no asset dependency, so it works in an empty
project) into the unsaved transient level, then proves it landed by inspecting
the level. Nothing is saved to disk, so editor-quit is a full reset — no
per-test cleanup needed.
"""

from harness.coverage import covers

POINT_LIGHT = "/Script/Engine.PointLight"


def _persistent_actor_count(bridge) -> int:
    info = bridge.expect("level_inspect")
    return int(info["persistent_level"]["actor_count"])


@covers("actor_spawn")
def test_actor_spawn_dry_run_does_not_mutate(mcp):
    """dry_run validates without spawning — the actor count must be unchanged."""
    before = _persistent_actor_count(mcp)
    # TOOL kwarg is class_path (the tool layer maps it to the bridge 'class' param).
    result = mcp.expect("actor_spawn", {"class_path": POINT_LIGHT, "dry_run": True})
    assert result.get("dry_run") is True
    assert result["diff"]["actors_added"][0]["class"].endswith("PointLight")
    after = _persistent_actor_count(mcp)
    assert after == before, "dry_run must not change the level"


@covers("actor_spawn", "level_inspect", "actor_query")
def test_actor_spawn_then_inspect(mcp):
    """A real spawn must increment the level's actor count and be addressable."""
    before = _persistent_actor_count(mcp)

    # TOOL kwarg is class_path (the tool layer maps it to the bridge 'class' param).
    spawned = mcp.expect("actor_spawn", {
        "class_path": POINT_LIGHT,
        "name": "MCPTest_PointLight",
        "location": {"x": 100.0, "y": 200.0, "z": 300.0},
    })
    assert spawned["success"] is True
    assert spawned["actor"]["class"].endswith("PointLight")
    spawned_name = spawned["actor"]["name"]

    after = _persistent_actor_count(mcp)
    assert after == before + 1, "real spawn must add exactly one actor"

    # And it must be queryable by name through a different command path.
    found = mcp.expect("actor_query", {"name_pattern": "MCPTest_PointLight"})
    names = [a.get("name") for a in found.get("actors", [])]
    assert spawned_name in names, f"{spawned_name} not found in {names}"
