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
from harness.ops import ensure_absent, assert_ready, payload

NS = "/Game/__MCPTest__/ik"
RETARGETER = f"{NS}/RTG_MCPTest"


def _find_asset(bridge, class_filter: str):
    """First /Game asset of a given class, or None. Used to gate content-
    dependent ops (IK rigs, anim sequences, pose assets).

    NOTE: asset_list nests its fields under the AssetManager `data` envelope,
    so the result must be unwrapped via payload() — first_asset_of cannot see
    the nested list (this silently skipped every rig-gated test, even against
    a project that ships IK rigs). Prior-run "_MCP*" artifacts are excluded."""
    try:
        result = payload(bridge.expect("asset_list", {
            "directory_path": "/Game/",
            "recursive": True,
            "class_filter": class_filter,
        }))
    except Exception:
        return None
    for a in result.get("assets") or []:
        p = str(a.get("path", "")).split(".")[0]
        if p and "_MCP" not in p and "__MCPTest__" not in p:
            return p
    return None


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


@covers("ik_retarget_auto_map_chains", "ik_retarget_read")
def test_ik_retargeter_auto_map_chains(mcp, configured_retargeter):
    result = mcp.expect("ik_retarget_auto_map_chains", {
        "retargeter_path": configured_retargeter["path"],
        "match_type": "Fuzzy",
        "force_remap": True,
        "align_target_pose": True,
    })
    assert isinstance(result.get("chain_mappings"), list), result
    assert_ready(mcp)
    # Independent readback (ik_retarget_read, not the mutator's echo): with an
    # IDENTITY rig on both sides, fuzzy auto-map must wire every target chain
    # to its same-named source chain (verified live: 50/50 identity mappings).
    rb = mcp.expect("ik_retarget_read", {"retargeter_path": configured_retargeter["path"]})
    mappings = rb.get("chain_mappings") or []
    assert mappings, rb
    unmapped = [m for m in mappings if m.get("source_chain") != m.get("target_chain")]
    assert not unmapped, f"identity auto-map left non-identity mappings: {unmapped}"


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
    # ik_retarget_read does not surface op settings and the 5.7 FInstancedStruct
    # op stack has no Python glue, so the readback is a SECOND setter call
    # writing an unrelated field: its response reports the STORED settings
    # struct, which must reflect the first call's write (the physics-constraint
    # from-value precedent).
    result = mcp.expect("ik_retarget_set_pelvis_settings", {
        "retargeter_path": configured_retargeter["path"],
        "scale_vertical": 0.25,
    })
    assert result.get("success") is not False, result
    written = result.get("written_fields") or []
    # Entries are "key=value" strings (e.g. "scale_vertical=0.2500").
    assert any(str(w).startswith("scale_vertical=") for w in written), result
    assert result.get("scale_vertical") == pytest.approx(0.25), result
    # Independent re-read: a call that writes ONLY rotation_alpha must report
    # the previously-stored scale_vertical.
    reread = mcp.expect("ik_retarget_set_pelvis_settings", {
        "retargeter_path": configured_retargeter["path"],
        "rotation_alpha": 1.0,
    })
    assert reread.get("scale_vertical") == pytest.approx(0.25), reread


@covers("ik_retarget_set_root_motion_settings")
def test_set_ik_retargeter_root_motion_settings(mcp, configured_retargeter):
    result = mcp.expect("ik_retarget_set_root_motion_settings", {
        "retargeter_path": configured_retargeter["path"],
        "root_height_source": "snap_to_ground",
    })
    assert result.get("success") is not False, result
    written = result.get("written_fields") or []
    assert any(str(w).startswith("root_height_source=") for w in written), result
    assert result.get("root_height_source") == "snap_to_ground", result
    # Independent re-read via a call writing only an unrelated boolean: the
    # stored Root Motion op settings must still report snap_to_ground.
    reread = mcp.expect("ik_retarget_set_root_motion_settings", {
        "retargeter_path": configured_retargeter["path"],
        "maintain_offset_from_pelvis": True,
    })
    assert reread.get("root_height_source") == "snap_to_ground", reread


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


def _rig_compatible_sequence(mcp, rig_path: str):
    """Find a UAnimSequence compatible with the rig's skeleton by walking the
    typed chain rig -> preview_mesh (ik_rig_list_chains) -> skeleton
    (anim_skeletal_mesh_inspect) -> anim_list_sequences(skeleton_path).
    Returns a package path or None. Prior-run "_MCP*" artifacts are excluded."""
    chains = mcp.expect("ik_rig_list_chains", {"ik_rig_path": rig_path})
    mesh = str(chains.get("preview_mesh", "")).split(".")[0]
    if not mesh:
        return None
    info = payload(mcp.expect("anim_skeletal_mesh_inspect", {"path": mesh}))
    skeleton = str(info.get("skeleton", "")).split(".")[0]
    if not skeleton:
        return None
    listed = mcp.expect("anim_list_sequences", {"skeleton_path": skeleton})
    for entry in listed.get("anim_sequences") or []:
        p = str(entry.get("path", "")).split(".")[0]
        if p.startswith("/Game/") and "_MCP" not in p and "__MCPTest__" not in p:
            return p
    return None


@covers("ik_retarget_run_batch", "ik_rig_list_chains")
def test_ik_retarget_run_batch(mcp, configured_retargeter):
    """Duplicate-and-retarget a namespaced dupe of a rig-compatible sequence.
    Deep assertion: the batch's new assets exist in the ASSET REGISTRY
    (asset_list — independent of the mutator's echo). Note the engine's
    DuplicateAndRetarget drops outputs at the /Game content root and does NOT
    save them to disk (verified live), so the registry is the observable and
    every output is deleted in finally."""
    src = _rig_compatible_sequence(mcp, configured_retargeter["rig"])
    if not src:
        pytest.skip("no rig-compatible UAnimSequence to duplicate-and-retarget")
    dupe = f"{NS}/Seq_BatchSrc"
    ensure_absent(mcp, dupe)
    mcp.expect("asset_duplicate", {"source_path": src, "destination_path": dupe})
    new_paths = []
    try:
        result = mcp.expect("ik_retarget_run_batch", {
            "retargeter_path": configured_retargeter["path"],
            "source_animations": [dupe],
            "name_suffix": "_MCPRetarget",
            "include_referenced_assets": False,
            "overwrite_existing": False,
        })
        new_paths = [str(p).split(".")[0] for p in (result.get("new_assets") or [])]
        assert new_paths, f"batch retarget produced no new assets: {result}"
        # Independent registry readback: each new asset enumerates with the
        # right class in its containing folder.
        for pkg in new_paths:
            folder = pkg.rsplit("/", 1)[0] or "/Game"
            listing = payload(mcp.expect("asset_list", {
                "directory_path": folder + "/", "recursive": False,
                "class_filter": "AnimSequence"}))
            found = {str(a.get("path", "")).split(".")[0] for a in (listing.get("assets") or [])}
            assert pkg in found, f"batch output {pkg} not in registry listing {sorted(found)}"
        assert_ready(mcp)
    finally:
        for pkg in new_paths:
            ensure_absent(mcp, pkg)
        ensure_absent(mcp, dupe)
