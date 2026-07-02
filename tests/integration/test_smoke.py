"""Liveness smoke tests — prove the editor booted and the bridge answers.

Mirrors the manual smoke sequence in docs/DEBUGGING.md: mcp_status -> ping ->
a read tool (asset_list).
"""

from harness.coverage import covers


@covers("mcp_status")
def test_mcp_status_ready(mcp):
    # mcp_status returns the bridge envelope unchanged; use .call (not .expect) so
    # the {status, result} shape is preserved for these assertions.
    resp = mcp.call("mcp_status", {})
    assert resp["status"] == "success", resp
    result = resp["result"]
    assert result["ready"] is True
    assert result["phase"] == "interactive"


@covers("ping")
def test_ping(bridge):
    # ping is a bridge-internal liveness probe — there is NO standalone MCP tool
    # for it, so this one stays at the bridge level.
    resp = bridge.ping()
    assert resp["status"] == "success", resp


@covers("asset_list")
def test_list_assets_read_path(mcp):
    # A read-only command exercises the full game-thread dispatch path without
    # mutating anything. Root /Game is always listable, even in an empty project.
    # (list_assets is the bridge command; asset_list is a Python-only alias.)
    result = mcp.expect("asset_list", {"directory_path": "/Game/", "recursive": True})
    assert isinstance(result, dict)
