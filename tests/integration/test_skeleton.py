"""Skeleton domain — socket CRUD on a USkeleton, read back via the list op.
Driven through the real MCP server (the `mcp` fixture calls tools by name).

Self-contained: operates on the engine's stock skeleton
``/Engine/EngineMeshes/SkeletalCube_Skeleton`` (confirmed present in every
install). Sockets are added/modified/removed entirely IN MEMORY — we never call
``save_asset`` on the engine package, so nothing on disk is mutated; each test
restores the skeleton to its starting socket set via the add/remove pair and is
idempotent (a stale test socket from a crashed prior run is removed first).

Pattern for every test: arrange prerequisite state -> dispatch the op (raises on
a non-success envelope) -> assert the resulting state via list_skeleton_sockets.
"""

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready, first_asset_of

# Stock engine assets (present out of the box; never saved/mutated on disk).
SKELETON = "/Engine/EngineMeshes/SkeletalCube_Skeleton"
MESH = "/Engine/EngineMeshes/SkeletalCube"
SOCKET = "MCPTestSocket"


# ── local helpers ────────────────────────────────────────────────────────────

def _remove_socket_quietly(bridge, name=SOCKET):
    """Idempotency guard: drop a leftover test socket, ignoring 'not found'."""
    try:
        bridge.command("anim_skeleton_remove_socket",
                       {"skeleton_path": SKELETON, "socket_name": name})
    except Exception:
        pass


def _socket_entries(bridge):
    result = bridge.expect("anim_skeleton_list_sockets", {"skeleton_path": SKELETON})
    sockets = result.get("sockets")
    assert isinstance(sockets, list), result
    return sockets


def _socket_names(bridge):
    return {s.get("socket_name") for s in _socket_entries(bridge)}


@pytest.fixture
def socket_bone(mcp):
    """A real bone name on the SkeletalCube skeleton to parent the socket to.

    Discovered from the skeletal mesh's ref skeleton (always available — it is
    not render-data gated, so it works under -nullrhi). Falls back to "Root"
    if discovery comes back empty (the add handler defers bone validation, so a
    plausible name still round-trips through list_skeleton_sockets)."""
    info = mcp.expect("anim_skeletal_mesh_inspect", {"path": MESH})
    bones = info.get("bones") or []
    return bones[0].get("name") if bones else "Root"


def _add_test_socket(bridge, bone, location_z=10.0):
    return bridge.expect("anim_skeleton_add_socket", {
        "skeleton_path": SKELETON,
        "socket_name": SOCKET,
        "bone_name": bone,
        "location_x": 0.0, "location_y": 0.0, "location_z": location_z,
        "rotation_pitch": 0.0, "rotation_yaw": 0.0, "rotation_roll": 0.0,
        "scale_x": 1.0, "scale_y": 1.0, "scale_z": 1.0,
        "dry_run": False,
    })


# ── tests ─────────────────────────────────────────────────────────────────────

@covers("anim_skeleton_add_socket", "anim_skeleton_list_sockets")
def test_add_skeleton_socket_appears_in_list(mcp, socket_bone):
    _remove_socket_quietly(mcp)  # start from a known-clean state
    result = _add_test_socket(mcp, socket_bone, location_z=12.0)
    assert result.get("success") is not False, result
    try:
        names = _socket_names(mcp)
        assert SOCKET in names, f"socket not listed after add: {names}"
        # the listed entry should report the bone we attached to
        entry = next(s for s in _socket_entries(mcp) if s.get("socket_name") == SOCKET)
        assert entry.get("bone_name") == socket_bone, entry
    finally:
        _remove_socket_quietly(mcp)  # leave the engine skeleton as we found it


@covers("anim_skeleton_modify_socket")
def test_modify_skeleton_socket_updates_value(mcp, socket_bone):
    _remove_socket_quietly(mcp)
    _add_test_socket(mcp, socket_bone, location_z=5.0)
    try:
        mcp.expect("anim_skeleton_modify_socket", {
            "skeleton_path": SKELETON,
            "socket_name": SOCKET,
            "location_z": 42.0,
        })
        entry = next(s for s in _socket_entries(mcp) if s.get("socket_name") == SOCKET)
        assert abs(float(entry.get("location_z")) - 42.0) < 1e-3, entry
    finally:
        _remove_socket_quietly(mcp)


@covers("anim_skeleton_remove_socket")
def test_remove_skeleton_socket_is_gone(mcp, socket_bone):
    _remove_socket_quietly(mcp)
    _add_test_socket(mcp, socket_bone)
    assert SOCKET in _socket_names(mcp), "precondition: socket should exist"

    result = mcp.expect("anim_skeleton_remove_socket", {
        "skeleton_path": SKELETON,
        "socket_name": SOCKET,
    })
    assert result.get("removed_socket") == SOCKET, result
    assert "remaining_sockets" in result, result
    assert SOCKET not in _socket_names(mcp), "socket still listed after remove"
    assert_ready(mcp)
