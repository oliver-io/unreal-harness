"""Level lifecycle: level_new -> level_save_as -> level_save -> level_load.

HAZARD — why this module is launch-mode only. The C++ handlers
(``MCPLevelCommands.cpp``) switch worlds WITHOUT saving the outgoing one:
``level_new`` passes ``bSaveExisting=false`` to ``NewBlankMap`` /
``NewMapFromTemplate`` and ``level_load`` calls
``UEditorLoadingAndSavingUtils::LoadMap`` with no save prompt — so switching
levels DISCARDS the open map's unsaved changes. Under ``--ue-attach`` the
editor is the SHARED dev editor and the open map is other agents' live WIP
(a 2026-07-02 probe found the map plus 16 dirty content packages), so this
module SKIPS in attach mode and runs only when the session owns a disposable
fixture editor (the default launch mode of ``tests/run.ps1``).

``level_save`` / ``level_save_as`` are surgical — the handlers call
``SaveMap(World, Package)`` on the current map package only, never a save-all —
but they are exercised here anyway so one guard covers the whole module.

Every test restores the original open level in teardown (``level_guard``),
even on assertion failure, then deletes the saved test map.
"""

import pytest

from harness import config
from harness.coverage import covers

TEST_MAP = f"/Game/{config.TEST_CONTENT_NAMESPACE}/levels/L_MCPLifecycle"
MISSING_MAP = f"/Game/{config.TEST_CONTENT_NAMESPACE}/levels/L_DoesNotExist"
POINT_LIGHT = "/Script/Engine.PointLight"


@pytest.fixture(autouse=True)
def _launch_mode_only(request):
    if request.config.getoption("--ue-attach"):
        pytest.skip(
            "level_new/level_load discard the open map's unsaved changes — "
            "unsafe against a shared attached editor; runs in launch mode only"
        )


def _current_package(mcp) -> str:
    """The open level's package path via level_inspect (independent readback)."""
    return str(mcp.expect("level_inspect")["path"]).split(".")[0]


@pytest.fixture
def level_guard(mcp):
    """Snapshot the open level; ALWAYS restore it, then delete the test map.

    Best-effort by design — teardown must never fail the run. If the baseline
    was a transient untitled world (no on-disk package to reload), a fresh
    blank map is the equivalent baseline.
    """
    original = _current_package(mcp)
    try:
        yield original
    finally:
        restored = False
        try:
            if original.startswith("/Game/"):
                resp = mcp.call("level_load", {"package_path": original})
                restored = resp.get("status") == "success"
        except Exception:
            pass
        if not restored:
            try:
                mcp.call("level_new", {})
            except Exception:
                pass
        try:
            mcp.call("asset_delete", {"asset_path": TEST_MAP, "force": True})
        except Exception:
            pass


@covers("level_new", "level_inspect")
def test_level_new_blank_replaces_world_with_transient_map(mcp, level_guard):
    """level_new must swap in a fresh transient (/Temp/) world."""
    before = _current_package(mcp)

    created = mcp.expect("level_new")
    assert created["created"] is True
    new_pkg = created["package_path"]
    assert new_pkg != before, "level_new must replace the open world"
    assert new_pkg.startswith("/Temp/"), f"expected a transient package, got {new_pkg}"

    # Independent readback: the editor's active world actually switched.
    info = mcp.expect("level_inspect")
    assert str(info["path"]).split(".")[0] == new_pkg
    assert info["name"] == created["map_name"]


@covers("level_save")
def test_level_save_refuses_transient_untitled_level(mcp, level_guard):
    """A /Temp/ untitled world has no on-disk home — level_save must refuse
    with invalid_path and point at level_save_as (the documented contract)."""
    mcp.expect("level_new")

    resp = mcp.call("level_save")
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "invalid_path", resp


@covers("level_save_as", "level_save", "level_load")
def test_level_save_as_save_and_load_roundtrip(mcp, level_guard):
    """The full disk round-trip: save_as creates the .umap and rebinds the
    active world; save rewrites it; load restores the persisted state."""
    mcp.expect("level_new")

    # save_as: the transient world lands on disk at the named /Game/ path...
    saved = mcp.expect("level_save_as", {"package_path": TEST_MAP})
    assert saved["saved"] is True
    assert saved["package_path"] == TEST_MAP
    umap = config.uasset_disk_path(TEST_MAP, ext=".umap")
    assert umap.exists() and umap.stat().st_size > 0, f"no .umap at {umap}"
    # ...and becomes the ACTIVE level (independent readback).
    assert _current_package(mcp) == TEST_MAP

    # Mutate, then level_save: the change must reach the disk package.
    spawned = mcp.expect("actor_spawn", {
        "class_path": POINT_LIGHT,
        "name": "MCPTest_LevelPersist",
    })
    spawned_name = spawned["actor"]["name"]
    mtime_before = umap.stat().st_mtime_ns
    resaved = mcp.expect("level_save")
    assert resaved["saved"] is True
    assert resaved["package_path"] == TEST_MAP
    assert umap.stat().st_mtime_ns != mtime_before, "level_save must rewrite the .umap"

    # Switch away, then load back from disk.
    mcp.expect("level_new")
    assert _current_package(mcp) != TEST_MAP
    loaded = mcp.expect("level_load", {"package_path": TEST_MAP})
    assert loaded["loaded"] is True
    assert loaded["package_path"] == TEST_MAP
    assert _current_package(mcp) == TEST_MAP

    # Deep observation: the saved actor survived the disk round-trip.
    found = mcp.expect("actor_query", {"name_pattern": "MCPTest_LevelPersist"})
    names = [a.get("name") for a in found.get("actors", [])]
    assert spawned_name in names, f"{spawned_name} not found in {names}"


@covers("level_load")
def test_level_load_missing_map_is_asset_not_found(mcp, level_guard):
    """A missing package must fail cleanly (asset_not_found) WITHOUT switching
    the open world (the handler pre-validates before touching LoadMap)."""
    before = _current_package(mcp)

    resp = mcp.call("level_load", {"package_path": MISSING_MAP})
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "asset_not_found", resp
    assert _current_package(mcp) == before, "a failed load must not switch worlds"
