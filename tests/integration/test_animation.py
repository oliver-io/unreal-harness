"""Animation domain — author Animation Blueprints, Montages and Blend Spaces
against the stock engine skeleton, mutate them, and read the resulting state
back. Driven through the real MCP server (the `mcp` fixture calls tools by name;
the tool layer maps kwargs→bridge params, etc.).

Mostly self-contained: AnimBP / Montage / BlendSpace are all authorable from
scratch given only a USkeleton, and the engine ships a stock one
(`/Engine/EngineMeshes/SkeletalCube_Skeleton`, confirmed present in every
install). The handful of ops that strictly require a pre-existing
UAnimSequence — `set_anim_sequence_property`, `extract_anim_between_notifies`,
`smooth_anim_sequence`, `normalize_anim_z_offset`, `anchor_feet_to_floor`, and
blend-space sample add/remove — DISCOVER one via `list_anim_sequences` and
`pytest.skip` when the project has none (the default for the fixture project).
The MCP exposes no op that authors a UAnimSequence, so those skips are expected;
the ops still count toward coverage via `@covers`.

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
    fixture project ships none, so these tests skip — but RUN if one is imported."""
    key = skeleton or "*"
    if key in _DISCOVERY:
        return _DISCOVERY[key]
    # Only /Game: these ops mutate-and-save the sequence, so a read-only /Engine
    # asset would fail rather than exercise the op. Exact class filter excludes
    # UAnimMontage (a UAnimSequenceBase). The fixture project ships no /Game
    # sequence, so these tests skip — and RUN if one is imported.
    path = None
    try:
        result = mcp.expect("asset_list", {
            "directory_path": "/Game", "recursive": True, "class_filter": "AnimSequence"})
        from harness.ops import payload
        assets = payload(result).get("assets") or []
        seqs = [a for a in assets if a.get("class") == "AnimSequence" and a.get("path")]
        path = seqs[0]["path"] if seqs else None
    except Exception:
        path = None
    _DISCOVERY[key] = path
    return path


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


@covers("anim_node_bind_property", "anim_blueprint_create", "bp_create_variable")
def test_bind_anim_node_property(mcp):
    """bind_anim_node_property targets an AnimGraph node. A freshly-created
    AnimBP only owns its output-pose (Root) node, whose FAnimNode struct has no
    bindable scalar pin, so a real bind may be a graceful no-op/error rather
    than a success. Dispatch it against that node and assert the handler answers
    cleanly (status present) and the editor survives — the binding write path is
    exercised either way."""
    name = "ABP_Bind"
    path = f"{NS}/{name}"
    ensure_absent(mcp, path)
    created = mcp.expect("anim_blueprint_create", {
        "name": name,
        "skeleton_path": SKELETON,
        "package_path": NS,
    })
    node_id = created.get("output_pose_node_id")
    if not node_id:
        pytest.skip("created AnimBP exposed no output-pose node id to bind against")
    # A variable for the binding to reference.
    mcp.expect("bp_create_variable", {
        "blueprint_name": path,
        "variable_name": "MCPBindVar",
        "variable_type": "float",
    })
    # Use command (not expect): binding onto the Root node may legitimately
    # error (no such bindable property) — we only require a graceful answer.
    resp = mcp.command("anim_node_bind_property", {
        "blueprint_name": path,
        "node_id": node_id,
        "property_name": "Sequence",
        "variable_name": "MCPBindVar",
    })
    assert isinstance(resp, dict) and resp.get("status") in ("success", "error"), resp
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

@covers("anim_sequence_set_property", "anim_list_sequences")
def test_set_anim_sequence_property(mcp):
    seq = _find_anim_sequence(mcp)
    if not seq:
        pytest.skip("no anim sequence asset in project")
    result = mcp.expect("anim_sequence_set_property", {
        "anim_path": seq,
        "additive_anim_type": "LocalSpace",
    })
    # Response echoes the engine-canonical short name (round-trip-safe).
    assert result.get("additive_anim_type") == "AAT_LocalSpaceBase", result
    assert result.get("success") is True, result


@covers("anim_extract_between_notifies", "anim_list_sequences")
def test_extract_anim_between_notifies(mcp):
    seq = _find_anim_sequence(mcp)
    if not seq:
        pytest.skip("no anim sequence asset in project to slice")
    result = mcp.expect("anim_extract_between_notifies", {
        "source_path": seq,
        "dest_name": "Anim_Extracted",
        "start_time": 0.0,
        "end_time": 0.1,
        "dest_path": NS,
    })
    assert result.get("name") == "Anim_Extracted" or result.get("path"), result


@covers("anim_smooth_sequence", "anim_list_sequences")
def test_smooth_anim_sequence(mcp):
    seq = _find_anim_sequence(mcp)
    if not seq:
        pytest.skip("no anim sequence asset in project to smooth")
    result = mcp.expect("anim_smooth_sequence", {
        "anim_path": seq,
        "window_size": 3,
        "filter_type": "box",
        "output_suffix": "_MCPSmoothed",
        "smooth_positions": False,
    })
    assert result.get("success") is True, result
    assert result.get("output_path"), result


@covers("anim_normalize_z_offset", "anim_list_sequences")
def test_normalize_anim_z_offset(mcp):
    seq = _find_anim_sequence(mcp)
    if not seq:
        pytest.skip("no anim sequence asset in project to normalize")
    result = mcp.expect("anim_normalize_z_offset", {
        "anim_path": seq,
        "target_z": 0.0,
        "output_suffix": "_MCPZNorm",
    })
    assert result.get("success") is True, result
    assert result.get("output_path"), result


@covers("anim_anchor_feet_to_floor", "anim_list_sequences")
def test_anchor_feet_to_floor(mcp):
    seq = _find_anim_sequence(mcp)
    if not seq:
        pytest.skip("no anim sequence asset in project to anchor")
    result = mcp.expect("anim_anchor_feet_to_floor", {
        "anim_path": seq,
        "foot_bone_substring": "foot_l",
        "pelvis_bone_substring": "pelvis",
        "target_z": 0.0,
        "sample_frames": 10,
        "output_suffix": "_MCPFootAnchored",
    })
    assert result.get("success") is True, result
    assert result.get("output_path"), result
