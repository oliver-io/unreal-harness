"""Foliage read-only inspection: foliage_inspect (``MCPFoliageCommands.cpp``).

Arrange strategy — ZERO C++, via the py escape hatch (recipe proven live
2026-07-02/03, banked in docs/loops/tests/TASKS.md): build a TRANSIENT
``FoliageType_InstancedStaticMesh`` pointing at the engine Cube, then the
static BlueprintCallable ``unreal.InstancedFoliageActor.add_instances(world,
type, transforms)`` places instances (creating the level's canonical IFA on
demand). Nothing is saved; the arrange is entirely in-memory level state.

Observe — the op under test reads the placed state through the C++ handler's
own iteration (a DIFFERENT path from the py mutator):

- mode='types' (default): per-type aggregate — identity == mesh path,
  display name, exact instance_count, key properties.
- mode='instances': the EXACT arranged transforms, with honest paging
  (limit/offset/returned/truncated/total_instances).
- error contracts: mode='instances' without ``foliage_type`` →
  ``invalid_argument``; unmatched ``foliage_type`` → ``asset_not_found``;
  unknown ``mode`` → ``invalid_argument`` (raw-wire only: the server-side
  zod enum already refuses a bogus mode at the MCP layer, so the C++ gate is
  reachable only through the bridge).

Teardown (proven live 2026-07-03): ``add_instances`` COPIES the transient
type into the IFA as a local ``FoliageType_InstancedStaticMesh``, so
``remove_all_instances`` must be fed the IFA's OWN registered type from
``ifa.get_used_foliage_types()`` — the stashed transient handle silently
removes nothing. After removal an empty IFA remains; deleting it (IFA is not
a LandscapeProxy — the docs/BUGS.md landscape hazard does not apply) restores
the exact ``{ifa_count:0, total_types:0, total_instances:0}`` baseline.
Safety on a shared level: only Cube-mesh types are ever removed, only IFAs
that did not pre-exist are deleted, and the fixture skips if the level
already has Cube foliage (never touch someone's real content).
"""

import pytest

from harness.coverage import covers

CUBE = "/Engine/BasicShapes/Cube.Cube"
MISSING_TYPE = "MCPTest_NoSuchFoliageType"
IFA_CLASS = "/Script/Foliage.InstancedFoliageActor"

# Far corner, away from anything real in a shared attached level.
LOCS = [
    (100000.0, 100000.0, -5000.0),
    (100100.0, 100000.0, -5000.0),
    (100200.0, 100000.0, -5000.0),
]

_ARRANGE_PY = (
    "py import unreal; "
    "w = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world(); "
    "t = unreal.FoliageType_InstancedStaticMesh(); "
    "t.set_editor_property('mesh', unreal.load_asset('" + CUBE + "')); "
    "ts = ["
    + ", ".join(
        "unreal.Transform(location=[%r, %r, %r])" % loc for loc in LOCS
    )
    + "]; "
    "unreal.InstancedFoliageActor.add_instances(w, t, ts)"
)

# Remove ONLY Cube-mesh foliage types, via each IFA's OWN registered type
# objects (the transient arrange handle is a copy source, not the registered
# type — remove_all_instances with it is a silent no-op; proven live).
_TEARDOWN_PY = (
    "py import unreal; "
    "s = unreal.get_editor_subsystem(unreal.EditorActorSubsystem); "
    "w = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world(); "
    "ifas = [a for a in s.get_all_level_actors() if isinstance(a, unreal.InstancedFoliageActor)]; "
    "[unreal.InstancedFoliageActor.remove_all_instances(w, t) "
    " for ifa in ifas for t in ifa.get_used_foliage_types() "
    " if t.get_editor_property('mesh') and t.get_editor_property('mesh').get_path_name() == '" + CUBE + "']"
)


def _ifa_names(mcp):
    listing = mcp.expect("actor_query", {"class_filter": IFA_CLASS})
    return {a["name"] for a in listing["actors"]}


def _cube_entry(inspect_result):
    for e in inspect_result.get("types", []):
        if e.get("mesh") == CUBE:
            return e
    return None


@pytest.fixture
def foliage_instances(mcp):
    """Three Cube foliage instances placed via the py hatch; always removed.

    Yields the pre-arrange baseline foliage_inspect result. Skips (never
    mutates) if the shared level already contains Cube-mesh foliage — the
    teardown could not distinguish ours from real content.
    """
    baseline = mcp.expect("foliage_inspect", {})
    if _cube_entry(baseline) is not None:
        pytest.skip(
            "level already contains Cube-mesh foliage — arranging/removing "
            "would be indistinguishable from someone's real content"
        )
    pre_ifas = _ifa_names(mcp)
    mcp.expect("editor_console_exec", {"command": _ARRANGE_PY})
    try:
        yield baseline
    finally:
        # Teardown must never fail the run; each step is independent.
        try:
            mcp.call("editor_console_exec", {"command": _TEARDOWN_PY})
        except Exception:
            pass
        try:
            for name in _ifa_names(mcp) - pre_ifas:
                mcp.call("actor_delete", {"name": name})
        except Exception:
            pass


@covers("foliage_inspect")
def test_foliage_inspect_types_reports_arranged_type(mcp, foliage_instances):
    """mode='types' (the default) must aggregate the placed foliage: the Cube
    type appears with identity == mesh path, its display name, the exact
    placed-instance count, and the documented per-type property block."""
    baseline = foliage_instances
    result = mcp.expect("foliage_inspect", {})
    assert result["mode"] == "types"
    assert result["ifa_count"] >= 1
    assert result["total_types"] == baseline["total_types"] + 1
    assert result["total_instances"] == baseline["total_instances"] + len(LOCS)

    entry = _cube_entry(result)
    assert entry is not None, result
    assert entry["identity"] == CUBE          # ISM identity is the mesh path
    assert entry["display_name"] == "Cube"
    assert entry["instance_count"] == len(LOCS)
    # Key-property block is present and sane (UFoliageType defaults).
    assert entry["density"] == 100
    assert entry["radius"] == 0
    assert isinstance(entry["align_to_normal"], bool)
    assert isinstance(entry["random_yaw"], bool)
    assert set(entry["cull_distance"]) == {"min", "max"}


@covers("foliage_inspect")
def test_foliage_inspect_instances_pages_exact_transforms(mcp, foliage_instances):
    """mode='instances' must return the EXACT arranged transforms with honest
    paging: limit 2 → returned 2 + truncated, offset 2 → the final one."""
    page1 = mcp.expect(
        "foliage_inspect", {"mode": "instances", "foliage_type": CUBE, "limit": 2}
    )
    assert page1["mode"] == "instances"
    assert page1["foliage_type"] == CUBE
    assert page1["total_instances"] == len(LOCS)
    assert page1["returned"] == 2
    assert len(page1["instances"]) == 2
    assert page1["truncated"] is True

    page2 = mcp.expect(
        "foliage_inspect",
        {"mode": "instances", "foliage_type": CUBE, "limit": 2, "offset": 2},
    )
    assert page2["returned"] == 1
    assert page2["truncated"] is False

    got = page1["instances"] + page2["instances"]
    assert [i["index"] for i in got] == [0, 1, 2]
    locs = sorted((i["location"]["x"], i["location"]["y"], i["location"]["z"]) for i in got)
    for actual, arranged in zip(locs, sorted(LOCS)):
        for a, b in zip(actual, arranged):
            assert abs(a - b) <= 1.0, (locs, LOCS)
    for inst in got:
        assert set(inst["rotation"]) == {"pitch", "yaw", "roll"}
        assert set(inst["scale"]) == {"x", "y", "z"}


@covers("foliage_inspect")
def test_foliage_inspect_instances_without_type_is_invalid_argument(mcp):
    """mode='instances' with no foliage_type must refuse, not dump everything."""
    resp = mcp.call("foliage_inspect", {"mode": "instances"})
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "invalid_argument", resp


@covers("foliage_inspect")
def test_foliage_inspect_unmatched_type_is_asset_not_found(mcp):
    resp = mcp.call(
        "foliage_inspect", {"mode": "instances", "foliage_type": MISSING_TYPE}
    )
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "asset_not_found", resp


@covers("foliage_inspect")
def test_foliage_inspect_unknown_mode_is_invalid_argument(bridge):
    """C++ mode gate — reachable only over the raw wire (the MCP layer's zod
    enum already refuses a bogus mode before it reaches the bridge)."""
    resp = bridge.command("foliage_inspect", {"mode": "bogus"})
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "invalid_argument", resp
