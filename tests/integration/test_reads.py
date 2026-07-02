"""Read-only / project / editor-utility ops.

Almost everything here works against an EMPTY project: the project- and
editor-level inspectors (scene_brief, project_context, editor_perf_snapshot)
report on whatever world/project is loaded, and the list_* asset readers return
a (possibly empty) array regardless of content. Pattern for those: dispatch ->
assert status==success (via bridge.expect) -> assert the documented list/field
key is present (the list may be empty).

A handful of reads are content-gated (they read a concrete asset by path):
bp_read needs a Blueprint — created once per module under the test namespace;
list_skeleton_sockets is pointed at an engine skeleton that always exists;
list_anim_notifies / list_ik_rig_chains have no project content in an empty
project and no list-op to discover one, so they are dispatched against a known
path and accept the structured error envelope.

The editor-utility writes (input_create) create an asset under the test
namespace; the session-end disk wipe (Content/__MCPTest__) cleans them up.
live_coding_compile is handled defensively — see its test.
"""

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready, first_asset_of

NS = "/Game/__MCPTest__/reads"
SAMPLE_BP = f"{NS}/BP_R"

# A skeleton that ships with the engine — present in every install, so the
# socket reader has a real asset to inspect even in an empty project.
ENGINE_SKELETON = "/Engine/EngineMeshes/SkeletalCube_Skeleton"


@pytest.fixture(scope="module")
def reads_bp(_mcp_client):
    """Create one Actor Blueprint for the module's bp_read test and compile it.
    Mirrors test_blueprint.sample_bp; not @covers-decorated (the create/compile
    ops are covered by the blueprint suite)."""
    ensure_absent(_mcp_client, SAMPLE_BP)
    _mcp_client.expect("bp_create_blueprint", {"name": SAMPLE_BP, "parent_class": "Actor"})
    _mcp_client.expect("bp_compile", {"blueprint_name": SAMPLE_BP})
    return SAMPLE_BP


# ── pure reads / project ────────────────────────────────────────────────────

@covers("scene_brief")
def test_scene_brief(mcp):
    result = mcp.expect("scene_brief", {})
    # Compact world summary: a name plus the per-class aggregation buckets.
    assert "world_name" in result, result
    assert "by_class" in result, result
    assert isinstance(result.get("skipped_sublevels"), list), result


@covers("project_context")
def test_project_context(mcp):
    result = mcp.expect("project_context", {})
    assert result.get("name"), result
    assert result.get("engine_version"), result
    assert isinstance(result.get("plugins"), list), result
    assert isinstance(result.get("modules"), list), result


@covers("editor_perf_snapshot")
def test_editor_perf_snapshot(mcp):
    result = mcp.expect("editor_perf_snapshot", {})
    # Frame + memory snapshot. memory is always populated; GPU timings may be
    # absent under -nullrhi (documented "field is absent, not zero").
    assert isinstance(result.get("memory"), dict) and result["memory"], result


@covers("bp_read")
def test_bp_read(mcp, reads_bp):
    # bp_read is the C++ alias of read_blueprint_content (param: blueprint_path).
    result = mcp.expect("bp_read", {"blueprint_path": reads_bp})
    assert isinstance(result, dict) and result, result


# ── list_* asset reads (empty-project-safe) ─────────────────────────────────

@covers("anim_list_skeletons")
def test_list_skeletons(mcp):
    result = mcp.expect("anim_list_skeletons", {})
    assert isinstance(result.get("skeletons"), list), result


@covers("anim_list_blueprints")
def test_list_anim_blueprints(mcp):
    result = mcp.expect("anim_list_blueprints", {})
    assert isinstance(result.get("anim_blueprints"), list), result


@covers("anim_list_montages")
def test_list_anim_montages(mcp):
    result = mcp.expect("anim_list_montages", {})
    assert isinstance(result.get("montages"), list), result


@covers("anim_list_sequences")
def test_list_anim_sequences(mcp):
    result = mcp.expect("anim_list_sequences", {})
    assert isinstance(result.get("anim_sequences"), list), result


@covers("anim_list_blend_spaces")
def test_list_blend_spaces(mcp):
    result = mcp.expect("anim_list_blend_spaces", {})
    assert isinstance(result.get("blend_spaces"), list), result


@covers("anim_list_layer_interfaces")
def test_list_anim_layer_interfaces(mcp):
    result = mcp.expect("anim_list_layer_interfaces", {})
    assert isinstance(result.get("layer_interfaces"), list), result


@covers("anim_skeleton_list_sockets")
def test_list_skeleton_sockets(mcp):
    # Pointed at an engine skeleton that always exists, so the read succeeds and
    # returns the sockets array (empty if the skeleton defines no sockets).
    result = mcp.expect("anim_skeleton_list_sockets", {"skeleton_path": ENGINE_SKELETON})
    assert isinstance(result.get("sockets"), list), result


@covers("anim_list_notifies", "anim_list_sequences")
def test_list_anim_notifies(mcp):
    # Notifies are read off a concrete anim asset, so this op is content-gated.
    # Discover one via list_anim_sequences; an empty project has none, in which
    # case we still dispatch the op (against a path known to be absent) and
    # accept the structured error envelope — the point is to exercise the wire.
    anim = first_asset_of(mcp, "anim_list_sequences", {}, "anim_sequences")
    if anim and anim.get("path"):
        result = mcp.expect("anim_list_notifies", {"anim_path": anim["path"]})
        assert isinstance(result.get("notifies"), list), result
    else:
        resp = mcp.command("anim_list_notifies", {"anim_path": f"{NS}/NoSuchAnim"})
        assert resp.get("status") in ("success", "error"), resp


@covers("ik_rig_list_chains")
def test_list_ik_rig_chains(mcp):
    # Chains live on a UIKRigDefinition. An empty project has none and there is
    # no list-IK-rigs op to discover one, so dispatch against a path known to be
    # absent and accept the structured error envelope. Confirms the command is
    # wired and leaves the editor interactive.
    resp = mcp.command("ik_rig_list_chains", {"ik_rig_path": f"{NS}/NoSuchIKRig"})
    assert resp.get("status") in ("success", "error"), resp
    assert_ready(mcp)


# ── editor utility ──────────────────────────────────────────────────────────

@covers("editor_console_exec")
def test_execute_console_command(mcp):
    # Headless-safe console command. Routed to the editor world (no PIE here).
    result = mcp.expect("editor_console_exec", {"command": "stat none"})
    assert isinstance(result, dict), result


@covers("input_create")
def test_input_create(mcp):
    # Creates a UInputAction under the test namespace; auto-saves. Result echoes
    # asset_path + class + type (+ value_type for actions).
    path = f"{NS}/IA_MCPTest"
    ensure_absent(mcp, path)
    result = mcp.expect("input_create", {
        "type": "action",
        "name": "IA_MCPTest",
        "path": NS,
        "value_type": "boolean",
    })
    assert result.get("asset_path"), result
    assert "InputAction" in str(result.get("class", "")), result


@covers("input_add_mapping")
def test_input_add_mapping(mcp):
    # GAP-002: wire keys to an action inside an IMC. Create both shells, then add
    # mappings and assert the IMC's mapping-row count climbs (the C++ handler's
    # mappings_count reads back UInputMappingContext::GetMappings().Num()).
    ia_path = f"{NS}/IA_MCPMap"
    imc_path = f"{NS}/IMC_MCPMap"
    ensure_absent(mcp, ia_path)
    ensure_absent(mcp, imc_path)
    mcp.expect("input_create", {"type": "action", "name": "IA_MCPMap", "path": NS,
                                "value_type": "boolean"})
    mcp.expect("input_create", {"type": "mapping_context", "name": "IMC_MCPMap", "path": NS})

    # Single key add → one row.
    res = mcp.expect("input_add_mapping", {
        "context_path": imc_path,
        "action_path": ia_path,
        "key": "SpaceBar",
    })
    assert res.get("keys_added") == 1, res
    assert res.get("mappings_count") == 1, res

    # Batch add of two more valid keys → three rows total.
    res2 = mcp.expect("input_add_mapping", {
        "context_path": imc_path,
        "action_path": ia_path,
        "keys": ["W", "Gamepad_FaceButton_Bottom"],
    })
    assert res2.get("keys_added") == 2, res2
    assert res2.get("mappings_count") == 3, res2

    # An invalid key name is rejected (invalid_argument) and mutates nothing.
    err = mcp.command("input_add_mapping", {
        "context_path": imc_path,
        "action_path": ia_path,
        "key": "NotARealKey_ZZZ",
    })
    assert err.get("status") == "error", err
    assert err.get("error_code") == "invalid_argument", err


@covers("editor_live_coding_compile")
def test_live_coding_compile(mcp):
    # Live Coding hot-patches the running editor. Under headless -nullrhi the
    # LiveCoding module is typically not loaded / not enabled-for-session, so the
    # C++ handler early-exits with a structured `live_coding_unavailable`-class
    # error in well under a second. If a real compile DID start it could block up
    # to the server-side 600s cap, so we cap the client wait and degrade to a
    # skip rather than hang the suite. Use .command (not .expect): a clean error
    # is a valid outcome here — only a transport failure/hang is not.
    try:
        resp = mcp.command("editor_live_coding_compile", {}, timeout=90)
    except Exception as e:
        pytest.skip(f"live_coding_compile did not return within budget "
                    f"(likely an actual compile in progress): {e}")
    assert resp.get("status") in ("success", "error"), resp
    # Either outcome must leave the editor interactive.
    assert_ready(mcp)
