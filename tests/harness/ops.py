"""Small cross-domain test helpers built on the bridge client.

Keeping creates idempotent lets the suite run repeatedly against a long-lived
editor (e.g. `--ue-attach`) as well as a fresh one.
"""

from __future__ import annotations

from typing import Optional

from .bridge_client import BridgeClient, CommandError


def ensure_absent(bridge: BridgeClient, asset_path: str) -> None:
    """Delete an asset if it exists; ignore 'not found' and any delete failure.
    Call before a create to make the test re-runnable."""
    try:
        bridge.command("asset_delete", {"asset_path": asset_path, "force": True})
    except Exception:
        pass


def payload(result: dict) -> dict:
    """Unwrap the AssetManager-style ``{success, data:{...}}`` envelope.

    Some handlers (AssetManager-derived: inspect_skeletal_mesh, the asset CRUD
    ops, etc.) nest their real fields under ``result["data"]``; others put them
    at the top level. This returns the inner payload either way."""
    inner = result.get("data")
    return inner if isinstance(inner, dict) else result


def assert_ready(bridge: BridgeClient) -> None:
    """Crash guard: after a risky op, the editor must still be interactive."""
    assert bridge.is_ready(), "editor is no longer interactive (possible crash)"


def first_asset_of(bridge: BridgeClient, list_command: str, params: dict,
                   items_key: str = "") -> Optional[dict]:
    """Return the first item from a list_* op, or None if the list is empty.
    Used by content-gated tests to discover a usable skeleton/mesh/etc."""
    try:
        result = bridge.expect(list_command, params)
    except CommandError:
        return None
    for key in ([items_key] if items_key else ["items", "assets", "results", "sockets"]):
        seq = result.get(key)
        if isinstance(seq, list) and seq:
            return seq[0]
    # fall back: first list-valued field
    for v in result.values():
        if isinstance(v, list) and v:
            return v[0]
    return None
