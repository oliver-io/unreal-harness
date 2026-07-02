"""IK retargeting domain — author a UIKRetargeter, wire rigs/chains, tune ops,
and read the state back. Driven through the real MCP server (the `mcp` fixture
calls tools by name).

What runs without imported content: ``create_ik_retargeter`` makes a valid
(empty) UIKRetargeter and ``read_ik_retargeter`` inspects it, so those two are
exercised end-to-end against a stock project.

Everything else needs a UIKRigDefinition (source/target rig), an AnimSequence,
or a PoseAsset — none of which a stock project ships. There is also no MCP op
to *create* an IK Rig, so those tests discover prerequisites via ``list_assets``
and ``pytest.skip`` with a precise reason when absent. They still ``@covers``
their op so the coverage guard sees it.

Pattern for every test: arrange prerequisite state -> dispatch the op (raises on
a non-success envelope) -> assert the resulting state via a read/inspect op.
"""

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready, first_asset_of

NS = "/Game/__MCPTest__/ik"
RETARGETER = f"{NS}/RTG_MCPTest"


def _find_asset(bridge, class_filter: str):
    """First /Game asset of a given class, or None. Used to gate content-
    dependent ops (IK rigs, anim sequences, pose assets)."""
    item = first_asset_of(bridge, "asset_list", {
        "directory_path": "/Game/",
        "recursive": True,
        "class_filter": class_filter,
    }, items_key="assets")
    return item.get("path") if item else None


@pytest.fixture(scope="module")
def empty_retargeter(_mcp_client):
    """One unconfigured UIKRetargeter for the whole module (no rigs wired)."""
    ensure_absent(_mcp_client, RETARGETER)
    _mcp_client.expect("ik_retarget_create", {"asset_path": RETARGETER})
    return RETARGETER


@pytest.fixture(scope="module")
def configured_retargeter(_mcp_client):
    """A UIKRetargeter wired to an IK rig on both sides (identity retarget), so
    the op stack (chain mappings, pelvis/root-motion ops) exists. Skips the
    whole dependent set when the project ships no IKRigDefinition."""
    rig = _find_asset(_mcp_client, "IKRigDefinition")
    if not rig:
        pytest.skip("no IKRigDefinition assets to build a configured retargeter "
                    "(stock project ships none, and there is no create_ik_rig op)")
    path = f"{NS}/RTG_Configured"
    ensure_absent(_mcp_client, path)
    _mcp_client.expect("ik_retarget_create", {
        "asset_path": path,
        "source_ik_rig_path": rig,
        "target_ik_rig_path": rig,
    })
    return {"path": path, "rig": rig}


# ── Always-runnable: create + read ───────────────────────────────────────────

@covers("ik_retarget_create")
def test_create_ik_retargeter_writes_uasset_on_disk(mcp):
    path = f"{NS}/RTG_Created"
    ensure_absent(mcp, path)
    result = mcp.expect("ik_retarget_create", {"asset_path": path})
    assert result.get("success") is True
    assert "RTG_Created" in result.get("asset_path", ""), result
    # The handler SaveAsset()s on create, so the package must exist on disk.
    disk = config.uasset_disk_path(path)
    assert disk.is_file(), f"expected {disk} after create"


@covers("ik_retarget_read")
def test_read_ik_retargeter_empty(mcp, empty_retargeter):
    result = mcp.expect("ik_retarget_read", {"retargeter_path": empty_retargeter})
    assert "RTG_MCPTest" in result.get("retargeter_path", ""), result
    assert isinstance(result.get("chain_mappings"), list)
    # Unconfigured retargeter — neither side is wired to a rig.
    assert result.get("source_ik_rig_path", "") == ""
    assert result.get("target_ik_rig_path", "") == ""


# ── Rig-dependent: skip when the project ships no IK Rig ──────────────────────

@covers("ik_retarget_create", "ik_retarget_set_rigs", "ik_retarget_read")
def test_set_ik_retargeter_rigs(mcp):
    rig = _find_asset(mcp, "IKRigDefinition")
    if not rig:
        pytest.skip("no IKRigDefinition assets to set on a retargeter")
    path = f"{NS}/RTG_SetRigs"
    ensure_absent(mcp, path)
    mcp.expect("ik_retarget_create", {"asset_path": path})
    mcp.expect("ik_retarget_set_rigs", {
        "retargeter_path": path,
        "source_ik_rig_path": rig,
        "target_ik_rig_path": rig,
        "rebuild_ops": True,
    })
    rb = mcp.expect("ik_retarget_read", {"retargeter_path": path})
    assert rb.get("source_ik_rig_path"), rb
    assert rb.get("target_ik_rig_path"), rb


@covers("ik_retarget_auto_map_chains")
def test_ik_retargeter_auto_map_chains(mcp, configured_retargeter):
    result = mcp.expect("ik_retarget_auto_map_chains", {
        "retargeter_path": configured_retargeter["path"],
        "match_type": "Fuzzy",
        "force_remap": True,
        "align_target_pose": True,
    })
    assert isinstance(result.get("chain_mappings"), list), result
    assert_ready(mcp)


@covers("ik_retarget_set_chain_mapping", "ik_retarget_read")
def test_set_ik_retargeter_chain_mapping(mcp, configured_retargeter):
    path = configured_retargeter["path"]
    rb = mcp.expect("ik_retarget_read", {"retargeter_path": path})
    mappings = rb.get("chain_mappings") or []
    if not mappings:
        pytest.skip("configured retargeter exposes no target chains to map")
    target_chain = mappings[0]["target_chain"]
    # Identity rig: wire the chain to itself (a name that is guaranteed valid).
    mcp.expect("ik_retarget_set_chain_mapping", {
        "retargeter_path": path,
        "target_chain": target_chain,
        "source_chain": target_chain,
    })
    rb2 = mcp.expect("ik_retarget_read", {"retargeter_path": path})
    wired = {m["target_chain"]: m["source_chain"] for m in rb2.get("chain_mappings", [])}
    assert wired.get(target_chain) == target_chain, rb2


@covers("ik_retarget_align_bones")
def test_ik_retargeter_align_bones(mcp, configured_retargeter):
    result = mcp.expect("ik_retarget_align_bones", {
        "retargeter_path": configured_retargeter["path"],
        "source_or_target": "target",
        "reset_first": True,
        "excluded_bones": [],
    })
    assert result.get("success") is not False, result
    assert_ready(mcp)


@covers("ik_retarget_set_pelvis_settings")
def test_set_ik_retargeter_pelvis_settings(mcp, configured_retargeter):
    # Needs the Pelvis Motion op, which only exists once the default op stack is
    # built (create with rigs -> AddDefaultOps). Hence the configured fixture.
    result = mcp.expect("ik_retarget_set_pelvis_settings", {
        "retargeter_path": configured_retargeter["path"],
        "scale_vertical": 0.0,
    })
    assert result.get("success") is not False, result
    assert "written_fields" in result, result
    assert "scale_vertical" in result.get("written_fields", []), result


@covers("ik_retarget_set_root_motion_settings")
def test_set_ik_retargeter_root_motion_settings(mcp, configured_retargeter):
    result = mcp.expect("ik_retarget_set_root_motion_settings", {
        "retargeter_path": configured_retargeter["path"],
        "root_height_source": "snap_to_ground",
    })
    assert result.get("success") is not False, result
    assert "written_fields" in result, result


# ── Anim/pose-dependent: skip when the project ships no source content ────────

@covers("ik_retarget_import_pose_from_animation")
def test_import_ik_retargeter_pose_from_animation(mcp, configured_retargeter):
    anim = _find_asset(mcp, "AnimSequence")
    if not anim:
        pytest.skip("no UAnimSequence to import a retarget pose from")
    result = mcp.expect("ik_retarget_import_pose_from_animation", {
        "retargeter_path": configured_retargeter["path"],
        "anim_sequence_path": anim,
        "source_or_target": "source",
        "frame_index": 0,
        "make_current": True,
    })
    assert result.get("success") is not False, result


@covers("ik_retarget_import_pose_from_pose_asset")
def test_import_ik_retargeter_pose_from_pose_asset(mcp, configured_retargeter):
    pose = _find_asset(mcp, "PoseAsset")
    if not pose:
        pytest.skip("no UPoseAsset to import a retarget pose from")
    result = mcp.expect("ik_retarget_import_pose_from_pose_asset", {
        "retargeter_path": configured_retargeter["path"],
        "pose_asset_path": pose,
        "source_or_target": "source",
        "make_current": True,
    })
    assert result.get("success") is not False, result


@covers("ik_retarget_run_batch")
def test_ik_retarget_run_batch(mcp, configured_retargeter):
    anim = _find_asset(mcp, "AnimSequence")
    if not anim:
        pytest.skip("no source UAnimSequence to duplicate-and-retarget")
    result = mcp.expect("ik_retarget_run_batch", {
        "retargeter_path": configured_retargeter["path"],
        "source_animations": [anim],
        "name_suffix": "_MCPRetarget",
        "include_referenced_assets": True,
        "overwrite_existing": False,
    })
    assert isinstance(result.get("new_assets"), list), result
    assert_ready(mcp)
