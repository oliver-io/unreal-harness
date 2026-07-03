"""Animation domain — author Animation Blueprints, Montages and Blend Spaces
against the stock engine skeleton, mutate them, and read the resulting state
back. Driven through the real MCP server (the `mcp` fixture calls tools by name;
the tool layer maps kwargs→bridge params, etc.).

Mostly self-contained: AnimBP / Montage / BlendSpace are all authorable from
scratch given only a USkeleton, and the engine ships a stock one
(`/Engine/EngineMeshes/SkeletalCube_Skeleton`, confirmed present in every
install). The handful of ops that strictly require a pre-existing
UAnimSequence — `anim_sequence_set_property`, `anim_extract_between_notifies`,
`anim_smooth_sequence`, `anim_normalize_z_offset`, `anim_anchor_feet_to_floor`,
and blend-space sample add/remove — DISCOVER one via `asset_list`, duplicate it
into the test namespace (`asset_duplicate`) and act on the DUPE, so real
project content is never mutated and every output lands inside the namespace.
They `pytest.skip` when the project has none (the default for the fixture
project). The MCP exposes no op that authors a UAnimSequence from nothing, so
those skips are expected.

Created assets live under `/Game/__MCPTest__/anim/...`; the session-end disk
wipe (Content/__MCPTest__) cleans them up. Each create is preceded by
`ensure_absent` so the suite re-runs against a long-lived editor.

NOTE — tool names and kwargs are taken from the MCP tool layer (the `mcp`
fixture). Creation ops take a short `name` + `package_path` PAIR (not a full
asset path); the on-disk asset lands at `package_path/name`. The notify ops load
a UAnimSequenceBase, so they accept a UAnimMontage — which lets the notify
lifecycle RUN against a freshly-created montage with no UAnimSequence in the
project.

Pattern for every test: arrange prerequisite state -> dispatch the op (raises on
a non-success envelope) -> assert the resulting state via a read/inspect op.
"""

import re
from pathlib import Path

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready, first_asset_of

NS = "/Game/__MCPTest__/anim"

# Stock engine skeleton (present out of the box). Created AnimBPs/Montages/Blend
# Spaces target it; nothing on the engine package itself is mutated/saved.
SKELETON = "/Engine/EngineMeshes/SkeletalCube_Skeleton"

ABP_SAMPLE = "ABP_Sample"
MONTAGE_SAMPLE = "Montage_Sample"
BS_SAMPLE = "BS_Sample"

# Cache the discovered anim-sequence path across the module so the content-gated
# tests hit list_anim_sequences once rather than per test.
_DISCOVERY: dict = {}


def _find_anim_sequence(mcp, skeleton: str = ""):
    """Return the path of a TRUE UAnimSequence in the project, or None.

    Uses an exact class filter — NOT list_anim_sequences, which also returns
    UAnimMontage (a UAnimSequenceBase). The smooth/normalize/anchor/extract ops
    require a real UAnimSequence; a montage makes them fail rather than skip. The
    fixture project ships none, so these tests skip — but RUN if one is imported.

    Prior test-run artifacts (namespaced dupes, "_MCP*" output-suffix leftovers)
    are excluded so discovery is stable across re-runs against a live project."""
    key = skeleton or "*"
    if key in _DISCOVERY:
        return _DISCOVERY[key]
    # Only /Game: these ops mutate-and-save the sequence, so a read-only /Engine
    # asset would fail rather than exercise the op. Exact class filter excludes
    # UAnimMontage (a UAnimSequenceBase). The fixture project ships no /Game
    # sequence, so these tests skip — and RUN if one is imported.
    path = None
    try:
        if skeleton:
            # Skeleton-compatible discovery (blend-space samples reject
            # sequences from a different skeleton): anim_list_sequences
            # applies the registry Skeleton-tag filter that asset_list lacks.
            listed = mcp.expect("anim_list_sequences", {"skeleton_path": skeleton})
            assets = listed.get("anim_sequences") or []
        else:
            result = mcp.expect("asset_list", {
                "directory_path": "/Game", "recursive": True,
                "class_filter": "AnimSequence"})
            from harness.ops import payload
            assets = [a for a in (payload(result).get("assets") or [])
                      if a.get("class") == "AnimSequence"]
        seqs = [a for a in assets
                if a.get("path") and str(a["path"]).startswith("/Game/")
                and "_MCP" not in a["path"] and "__MCPTest__" not in a["path"]]
        path = seqs[0]["path"] if seqs else None
    except Exception:
        path = None
    _DISCOVERY[key] = path
    return path


def _live_uasset_disk_path(client, game_path: str) -> Path:
    """Attach-safe /Game/... -> Content/....uasset mapping using the live
    editor's own project root (B6 precedent — see test_asset.py/test_material.py)."""
    ctx = client.expect("project_context", {})
    root = Path(ctx["settings_paths"][0])
    pkg = game_path.split(".")[0]
    assert pkg.startswith("/Game/"), game_path
    return root / "Content" / (pkg[len("/Game/"):] + ".uasset")


def _dupe_sequence(mcp, dest_name: str):
    """Duplicate a discovered project UAnimSequence into the test namespace so
    the mutating sequence ops act on a TEST-OWNED asset (never real project
    content). Returns the dupe's package path, or None when the project ships
    no UAnimSequence (fixture default — callers pytest.skip)."""
    src = _find_anim_sequence(mcp)
    if not src:
        return None
    dest = f"{NS}/{dest_name}"
    ensure_absent(mcp, dest)
    mcp.expect("asset_duplicate", {"source_path": src.split(".")[0], "destination_path": dest})
    return dest


# ── module-scoped sample assets ─────────────────────────────────────────────

@pytest.fixture(scope="module")
def sample_abp(_mcp_client):
    """One Animation Blueprint for the whole module, targeting the engine skeleton."""
    path = f"{NS}/{ABP_SAMPLE}"
    ensure_absent(_mcp_client, path)
    _mcp_client.expect("anim_blueprint_create", {
        "name": ABP_SAMPLE,
        "skeleton_path": SKELETON,
        "package_path": NS,
    })
    return path


@pytest.fixture(scope="module")
def sample_montage(_mcp_client):
    """One AnimMontage for the whole module (no source sequence needed)."""
    path = f"{NS}/{MONTAGE_SAMPLE}"
    ensure_absent(_mcp_client, path)
    _mcp_client.expect("anim_montage_create", {
        "name": MONTAGE_SAMPLE,
        "skeleton_path": SKELETON,
        "package_path": NS,
        "slot_name": "DefaultSlot",
    })
    return path


# ── Animation Blueprint ─────────────────────────────────────────────────────

@covers("anim_blueprint_create", "anim_list_blueprints")
def test_create_anim_blueprint_writes_uasset_on_disk(mcp):
    name = "ABP_Created"
    path = f"{NS}/{name}"
    ensure_absent(mcp, path)
    result = mcp.expect("anim_blueprint_create", {
        "name": name,
        "skeleton_path": SKELETON,
        "package_path": NS,
    })
    assert result.get("success") is True, result
    # The created object path carries the .Object suffix; the short name must match.
    assert name in str(result.get("path")), result
    assert SKELETON in str(result.get("skeleton")), result
    # create_anim_blueprint auto-saves, so the .uasset must exist now.
    disk = config.uasset_disk_path(path)
    assert disk.is_file(), f"expected {disk} to exist after create_anim_blueprint"
    # Read-back via the discovery op: the new AnimBP must be enumerated.
    listed = mcp.expect("anim_list_blueprints", {})
    names = {b.get("name") for b in (listed.get("anim_blueprints") or [])}
    assert name in names, names


@covers("anim_blueprint_set_skeleton", "anim_blueprint_create", "anim_list_blueprints")
def test_set_anim_blueprint_skeleton(mcp, sample_abp):
    # Re-assert the target skeleton (recompiles the AnimBP through the same path
    # a true skeleton swap would; only one engine skeleton is available).
    result = mcp.expect("anim_blueprint_set_skeleton", {
        "blueprint_path": sample_abp,
        "skeleton_path": SKELETON,
    })
    assert result.get("success") is True, result
    assert SKELETON in str(result.get("skeleton")), result
    assert_ready(mcp)
    listed = mcp.expect("anim_list_blueprints", {})
    entry = next((b for b in (listed.get("anim_blueprints") or [])
                  if b.get("name") == ABP_SAMPLE), None)
    assert entry, f"{ABP_SAMPLE} not listed"
    # target_skeleton tag is the asset-registry form; it must name our skeleton.
    assert "SkeletalCube_Skeleton" in str(entry.get("target_skeleton")), entry


@covers("anim_node_bind_property", "anim_blueprint_create", "bp_create_variable",
        "add_blueprint_node", "bp_list_node_pins")  # bp_add_node's wire name
def test_bind_anim_node_property(mcp):
    """Bind a SequencePlayer node's PlayRate (a pin-optional FAnimNode property)
    to a float variable, and PROVE the bind through an independent reader:
    binding toggles ShowPinForProperties + ReconstructNode, which materializes
    the PlayRate pin — absent from bp_list_node_pins before the bind, present
    after (verified live). The PropertyBindings TMap itself is not observable:
    no typed reader exposes it and the py hatch cannot read a
    TMap<FName, FAnimGraphNodePropertyBinding> (the value struct has no Python
    glue — verified live), so the reconstructed pin is the deepest available
    observation of the write."""
    name = "ABP_Bind"
    path = f"{NS}/{name}"
    ensure_absent(mcp, path)
    mcp.expect("anim_blueprint_create", {
        "name": name,
        "skeleton_path": SKELETON,
        "package_path": NS,
    })
    # A SequencePlayer node — FAnimNode_SequencePlayer.PlayRate is a real,
    # pin-optional (PinHiddenByDefault) bindable scalar.
    added = mcp.expect("bp_add_node", {
        "blueprint_name": path,
        "node_type": "SequencePlayer",
        "function_name": "AnimGraph",
    })
    node_id = added.get("node_id")
    assert node_id, added
    # A variable for the binding to reference.
    mcp.expect("bp_create_variable", {
        "blueprint_name": path,
        "variable_name": "MCPBindRate",
        "variable_type": "float",
    })

    def _pin_names():
        listed = mcp.expect("bp_list_node_pins", {
            "blueprint_name": path, "node_id": node_id, "graph_name": "AnimGraph"})
        return {p.get("name") for p in (listed.get("pins") or [])}

    # Before: the hidden-by-default PlayRate pin is not serialized.
    assert "PlayRate" not in _pin_names(), "PlayRate pin visible before binding"

    result = mcp.expect("anim_node_bind_property", {
        "blueprint_name": path,
        "node_id": node_id,
        "property_name": "PlayRate",
        "variable_name": "MCPBindRate",
        "graph_name": "AnimGraph",
    })
    assert result.get("success") is True, result
    assert result.get("pin_toggled_visible") is True, result

    # Independent readback: the bound pin now exists on the reconstructed node.
    assert "PlayRate" in _pin_names(), "bound PlayRate pin did not materialize"
    assert_ready(mcp)


# ── AnimMontage ─────────────────────────────────────────────────────────────

@covers("anim_montage_create", "anim_montage_read")
def test_create_and_read_montage(mcp):
    name = "Montage_Created"
    path = f"{NS}/{name}"
    ensure_absent(mcp, path)
    created = mcp.expect("anim_montage_create", {
        "name": name,
        "skeleton_path": SKELETON,
        "package_path": NS,
        "slot_name": "DefaultSlot",
    })
    assert created.get("success") is True, created
    assert name in str(created.get("path")), created
    # create_anim_montage auto-saves.
    disk = config.uasset_disk_path(path)
    assert disk.is_file(), f"expected {disk} to exist after create_anim_montage"

    read = mcp.expect("anim_montage_read", {"montage_path": path})
    assert name in str(read.get("path")), read
    assert "sections" in read, read
    assert isinstance(read.get("slot_tracks"), list), read
    assert "SkeletalCube_Skeleton" in str(read.get("skeleton")), read


@covers("anim_montage_add_section", "anim_montage_read")
def test_add_montage_section(mcp, sample_montage):
    result = mcp.expect("anim_montage_add_section", {
        "montage_path": sample_montage,
        "section_name": "S_Intro",
        "start_time": 0.0,
    })
    assert result.get("success") is True, result
    assert result.get("section_index", -1) >= 0, result
    read = mcp.expect("anim_montage_read", {"montage_path": sample_montage})
    names = {s.get("name") for s in (read.get("sections") or [])}
    assert "S_Intro" in names, names


@covers("anim_montage_set_blend", "anim_montage_read")
def test_set_montage_blend(mcp, sample_montage):
    mcp.expect("anim_montage_set_blend", {
        "montage_path": sample_montage,
        "blend_in_time": 0.25,
        "blend_out_time": 0.5,
    })
    read = mcp.expect("anim_montage_read", {"montage_path": sample_montage})
    assert read.get("blend_in_time") == pytest.approx(0.25), read
    assert read.get("blend_out_time") == pytest.approx(0.5), read


@covers("anim_montage_set_section_link", "anim_montage_add_section", "anim_montage_read")
def test_set_montage_section_link(mcp, sample_montage):
    # Two sections must exist before they can be linked.
    mcp.expect("anim_montage_add_section", {
        "montage_path": sample_montage, "section_name": "S_LinkA", "start_time": 0.0,
    })
    mcp.expect("anim_montage_add_section", {
        "montage_path": sample_montage, "section_name": "S_LinkB", "start_time": 0.0,
    })
    result = mcp.expect("anim_montage_set_section_link", {
        "montage_path": sample_montage,
        "section_name": "S_LinkA",
        "next_section_name": "S_LinkB",
    })
    assert result.get("success") is True, result
    read = mcp.expect("anim_montage_read", {"montage_path": sample_montage})
    entry = next((s for s in (read.get("sections") or []) if s.get("name") == "S_LinkA"), None)
    assert entry, read
    assert entry.get("next_section") == "S_LinkB", entry


# ── Anim Notifies (run against a montage — UAnimSequenceBase) ────────────────

@covers("anim_notify_add", "anim_list_notifies", "anim_notify_remove",
        "anim_montage_create")
def test_anim_notify_lifecycle(mcp):
    name = "Montage_Notify"
    path = f"{NS}/{name}"
    ensure_absent(mcp, path)
    mcp.expect("anim_montage_create", {
        "name": name, "skeleton_path": SKELETON, "package_path": NS,
    })

    added = mcp.expect("anim_notify_add", {
        "anim_path": path,
        "notify_class": "AnimNotify_PlaySound",
        "trigger_time": 0.0,
        "track_index": 0,
    })
    assert added.get("success") is True, added
    idx = added.get("notify_index")
    assert idx is not None, added

    listed = mcp.expect("anim_list_notifies", {"anim_path": path})
    assert listed.get("count", 0) >= 1, listed
    entry = next((n for n in (listed.get("notifies") or []) if n.get("index") == idx), None)
    assert entry, listed

    removed = mcp.expect("anim_notify_remove", {"anim_path": path, "notify_index": idx})
    assert removed.get("success") is not False, removed
    after = mcp.expect("anim_list_notifies", {"anim_path": path})
    assert idx not in {n.get("index") for n in (after.get("notifies") or [])}, after


# ── BlendSpace ──────────────────────────────────────────────────────────────

@covers("anim_blend_space_create", "anim_blend_space_read")
def test_create_and_read_blend_space(mcp):
    name = "BS_Created"
    path = f"{NS}/{name}"
    ensure_absent(mcp, path)
    created = mcp.expect("anim_blend_space_create", {
        "name": name,
        "skeleton_path": SKELETON,
        "is_2d": False,
        "package_path": NS,
        "x_axis_name": "Speed", "x_axis_min": 0.0, "x_axis_max": 100.0,
        "x_axis_grid_divisions": 4,
    })
    assert created.get("success") is True, created
    assert created.get("type") == "BlendSpace1D", created
    # create_blend_space auto-saves.
    disk = config.uasset_disk_path(path)
    assert disk.is_file(), f"expected {disk} to exist after create_blend_space"

    read = mcp.expect("anim_blend_space_read", {"blend_space_path": path})
    # read_blend_space's type label vocabulary differs from create's
    # ("BlendSpace1D" vs "2D"); just assert the asset round-trips with its axis.
    assert "type" in read, read
    assert read.get("x_axis", {}).get("name") == "Speed", read
    assert isinstance(read.get("samples"), list), read
    assert read.get("sample_count") == 0, read


@covers("anim_blend_space_add_sample", "anim_blend_space_remove_sample", "anim_blend_space_create",
        "anim_blend_space_read")
def test_blend_space_sample_lifecycle(mcp):
    seq = _find_anim_sequence(mcp, skeleton=SKELETON)
    if not seq:
        pytest.skip("no anim sequence asset in project (blend-space samples need one)")

    name = "BS_Samples"
    path = f"{NS}/{name}"
    ensure_absent(mcp, path)
    mcp.expect("anim_blend_space_create", {
        "name": name, "skeleton_path": SKELETON, "is_2d": False, "package_path": NS,
        "x_axis_name": "Speed", "x_axis_min": 0.0, "x_axis_max": 100.0,
        "x_axis_grid_divisions": 4,
    })

    added = mcp.expect("anim_blend_space_add_sample", {
        "blend_space_path": path, "anim_sequence": seq, "x": 50.0,
    })
    assert added.get("success") is True, added
    sample_idx = added.get("sample_index")
    assert sample_idx is not None, added

    read = mcp.expect("anim_blend_space_read", {"blend_space_path": path})
    assert read.get("sample_count", 0) >= 1, read

    removed = mcp.expect("anim_blend_space_remove_sample", {
        "blend_space_path": path, "sample_index": sample_idx,
    })
    assert removed.get("success") is True, removed
    after = mcp.expect("anim_blend_space_read", {"blend_space_path": path})
    assert after.get("sample_count", -1) == 0, after


# ── UAnimSequence-gated ops (skip when the project ships no sequence) ────────
# Each test duplicates a discovered project sequence into the namespace first
# (asset_duplicate) and acts on the DUPE — real project content is never
# mutated, and the output-suffix ops' new assets land inside the namespace.

@covers("anim_list_sequences")
def test_list_anim_sequences_enumerates_created_sequence(mcp):
    """Actually CALL anim_list_sequences (it was previously only @covers-tagged,
    never invoked) and prove it against independent state: a freshly-duplicated
    namespaced UAnimSequence must enumerate, the count must equal the list
    length, and every /Game AnimSequence known to the asset registry
    (asset_list — a different reader) must appear in its output. Also pins the
    documented bad-skeleton error contract."""
    seq = _dupe_sequence(mcp, "Seq_ListProbe")
    if not seq:
        pytest.skip("no anim sequence asset in project")
    try:
        listed = mcp.expect("anim_list_sequences", {})
        assert listed.get("success") is True, listed
        entries = listed.get("anim_sequences") or []
        assert listed.get("count") == len(entries), listed
        paths = {str(e.get("path", "")).split(".")[0] for e in entries}
        assert seq in paths, f"created dupe {seq} not enumerated: {sorted(paths)[:10]}"
        # Cross-check against the independent asset-registry reader.
        from harness.ops import payload
        registry = payload(mcp.expect("asset_list", {
            "directory_path": "/Game", "recursive": True,
            "class_filter": "AnimSequence"})).get("assets") or []
        reg_paths = {str(a.get("path", "")).split(".")[0] for a in registry
                     if a.get("class") == "AnimSequence"}
        assert reg_paths <= paths, f"registry sequences missing from list: {reg_paths - paths}"
        # Documented negative path: a bogus skeleton filter errors loudly.
        bad = mcp.command("anim_list_sequences", {"skeleton_path": f"{NS}/NoSuchSkeleton"})
        assert bad.get("status") == "error", bad
    finally:
        ensure_absent(mcp, seq)


@covers("anim_sequence_set_property")
def test_set_anim_sequence_property(mcp):
    seq = _dupe_sequence(mcp, "Seq_SetProp")
    if not seq:
        pytest.skip("no anim sequence asset in project")
    try:
        result = mcp.expect("anim_sequence_set_property", {
            "anim_path": seq,
            "additive_anim_type": "LocalSpace",
        })
        # Response echoes the engine-canonical short name (round-trip-safe).
        assert result.get("additive_anim_type") == "AAT_LocalSpaceBase", result
        assert result.get("success") is True, result
        # Independent readback via the sanctioned py console hatch: the loaded
        # UAnimSequence's AdditiveAnimType UPROPERTY now reports LocalSpaceBase
        # (no typed reader exposes sequence properties).
        name = seq.rsplit("/", 1)[-1]
        probe = mcp.expect("editor_console_exec", {"command": (
            "py import unreal; "
            f"s = unreal.load_object(None, '{seq}.{name}'); "
            "print('MCPTEST_AAT=' + (str(s.get_editor_property('additive_anim_type')) if s else 'NOTFOUND'))"
        )})
        m = re.search(r"MCPTEST_AAT=([^\r\n]+)", probe.get("output", ""))
        assert m, probe
        assert "AAT_LOCAL_SPACE_BASE" in m.group(1), m.group(1)
    finally:
        ensure_absent(mcp, seq)


@covers("anim_extract_between_notifies")
def test_extract_anim_between_notifies(mcp):
    seq = _dupe_sequence(mcp, "Seq_ExtractSrc")
    if not seq:
        pytest.skip("no anim sequence asset in project to slice")
    out = f"{NS}/Anim_Extracted"
    ensure_absent(mcp, out)
    try:
        result = mcp.expect("anim_extract_between_notifies", {
            "source_path": seq,
            "dest_name": "Anim_Extracted",
            "start_time": 0.0,
            "end_time": 0.1,
            "dest_path": NS,
        })
        assert result.get("success") is True, result
        assert result.get("name") == "Anim_Extracted", result
        # The handler SaveAsset()s the sliced clip — the new .uasset must exist
        # on disk (attach-safe path via the live editor's own project root).
        disk = _live_uasset_disk_path(mcp, out)
        assert disk.is_file(), f"expected {disk} after extract"
    finally:
        ensure_absent(mcp, out)
        ensure_absent(mcp, seq)


def _output_suffix_roundtrip(mcp, dupe_name: str, op: str, suffix: str, params: dict):
    """Shared deep assertion for the output-suffix ops: act on a namespaced
    dupe, assert the echoed output_path is the dupe+suffix INSIDE the
    namespace, then confirm the new .uasset on disk (the handlers SaveAsset the
    output — verified against MCPIKCommands.cpp)."""
    seq = _dupe_sequence(mcp, dupe_name)
    if not seq:
        pytest.skip("no anim sequence asset in project")
    expected_out = f"{seq}{suffix}"
    ensure_absent(mcp, expected_out)
    try:
        result = mcp.expect(op, {"anim_path": seq, "output_suffix": suffix, **params})
        assert result.get("success") is True, result
        out = str(result.get("output_path", "")).split(".")[0]
        assert out == expected_out, result
        disk = _live_uasset_disk_path(mcp, expected_out)
        assert disk.is_file(), f"expected {disk} after {op}"
    finally:
        ensure_absent(mcp, expected_out)
        ensure_absent(mcp, seq)


@covers("anim_smooth_sequence")
def test_smooth_anim_sequence(mcp):
    _output_suffix_roundtrip(mcp, "Seq_SmoothSrc", "anim_smooth_sequence", "_MCPSmoothed", {
        "window_size": 3,
        "filter_type": "box",
        "smooth_positions": False,
    })


@covers("anim_normalize_z_offset")
def test_normalize_anim_z_offset(mcp):
    _output_suffix_roundtrip(mcp, "Seq_ZNormSrc", "anim_normalize_z_offset", "_MCPZNorm", {
        "target_z": 0.0,
    })


@covers("anim_anchor_feet_to_floor")
def test_anchor_feet_to_floor(mcp):
    _output_suffix_roundtrip(mcp, "Seq_AnchorSrc", "anim_anchor_feet_to_floor", "_MCPFootAnchored", {
        "foot_bone_substring": "foot_l",
        "pelvis_bone_substring": "pelvis",
        "target_z": 0.0,
        "sample_frames": 10,
    })
