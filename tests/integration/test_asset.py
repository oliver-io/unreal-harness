"""Asset domain — create assets under the test namespace, then refactor them
through the AssetManager bridge (duplicate / rename / move / delete), open them,
walk their reference graph, fix up redirectors, rescan the content browser, and
import a texture from disk.

Source assets are real on-disk packages under /Game/__MCPTest__/asset/...; the
session-end disk wipe (Content/__MCPTest__) cleans them up. Creates are made
re-runnable with ensure_absent before each create.

Pattern for every test: arrange prerequisite state -> dispatch the op (raises on
a non-success envelope) -> assert the resulting state via a read-back (the op's
echoed paths, an on-disk uasset where we asked for one, or an expected-error
probe proving an asset is gone).

Note on envelope shape: the AssetManager handlers wrap their payload under a
`data` sub-object (CreateSuccessResponse), whereas asset_references /
content_browser_refresh / import_textures return their fields at the top level.
"""

import struct
import tempfile
import zlib
from pathlib import Path

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready

NS = "/Game/__MCPTest__/asset"


def _make_saved_bp(bridge, path: str) -> str:
    """Idempotently create + save an Actor Blueprint so it exists in the asset
    registry and on disk (a prerequisite for the refactor ops)."""
    ensure_absent(bridge, path)
    bridge.expect("bp_create_blueprint", {"name": path, "parent_class": "Actor"})
    bridge.expect("asset_save", {"asset_paths": [path]})
    return path


@pytest.fixture(scope="module")
def source_bp(_mcp_client):
    """One shared, saved source Blueprint for the read-only / copy tests."""
    return _make_saved_bp(_mcp_client, f"{NS}/BP_Source")


def _png_bytes(width: int = 4, height: int = 4, rgb=(200, 120, 60)) -> bytes:
    """A minimal valid 8-bit RGB PNG built with the stdlib (no Pillow needed)."""
    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # bit depth 8, color type 2 (RGB)
    row = b"\x00" + bytes(rgb) * width  # filter byte 0 + one scanline
    raw = row * height
    idat = zlib.compress(raw)
    return sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


@covers("asset_duplicate")
def test_duplicate_asset_writes_uasset(mcp, source_bp):
    dest = f"{NS}/BP_Dup"
    ensure_absent(mcp, dest)
    result = mcp.expect("asset_duplicate", {
        "source_path": source_bp,
        "destination_path": dest,
    })
    assert result.get("success") is True, result
    assert result["data"]["destination_path"] == dest, result

    # Save the copy, then the package file must exist at the mapped Content path.
    mcp.expect("asset_save", {"asset_paths": [dest]})
    disk = config.uasset_disk_path(dest)
    assert disk.is_file(), f"expected {disk} to exist after duplicate+save"


@covers("asset_duplicate")
def test_duplicate_asset_dry_run_does_not_create(mcp, source_bp):
    dest = f"{NS}/BP_DupDry"
    ensure_absent(mcp, dest)
    result = mcp.expect("asset_duplicate", {
        "source_path": source_bp,
        "destination_path": dest,
        "dry_run": True,
    })
    assert result.get("dry_run") is True, result
    assert result["diff"]["created"][0]["path"] == dest, result
    assert not config.uasset_disk_path(dest).exists(), "dry_run must not write a uasset"


@covers("asset_rename")
def test_rename_asset(mcp):
    src = _make_saved_bp(mcp, f"{NS}/BP_RenameSrc")
    new_name = "BP_RenameDst"
    dest = f"{NS}/{new_name}"
    ensure_absent(mcp, dest)

    result = mcp.expect("asset_rename", {"source_path": src, "new_name": new_name})
    assert result.get("success") is True, result
    assert result["data"]["new_path"] == dest, result

    mcp.expect("asset_save", {"asset_paths": [dest]})
    assert config.uasset_disk_path(dest).is_file(), "renamed asset uasset missing on disk"


@covers("asset_move")
def test_move_asset(mcp):
    src = _make_saved_bp(mcp, f"{NS}/BP_MoveSrc")
    dest_folder = f"{NS}/moved"
    dest = f"{dest_folder}/BP_MoveSrc"
    ensure_absent(mcp, dest)

    result = mcp.expect("asset_move", {
        "source_path": src,
        "destination_folder": dest_folder,
    })
    assert result.get("success") is True, result
    assert result["data"]["new_path"] == dest, result

    mcp.expect("asset_save", {"asset_paths": [dest]})
    assert config.uasset_disk_path(dest).is_file(), "moved asset uasset missing on disk"


@covers("asset_references")
def test_asset_references(mcp, source_bp):
    """Outbound dependency lookup over the asset reference graph (read-only)."""
    result = mcp.expect("asset_references", {
        "asset_path": source_bp,
        "direction": "outbound",
        "depth": 1,
    })
    assert result["direction"] == "outbound", result
    refs = result["references"]
    assert isinstance(refs, list), result
    assert result["returned_count"] == len(refs), result


@covers("asset_delete")
def test_delete_asset_then_absent(mcp):
    path = _make_saved_bp(mcp, f"{NS}/BP_DelSrc")

    result = mcp.expect("asset_delete", {"asset_path": path, "force": True})
    assert result.get("success") is True, result
    assert result["data"]["deleted_path"] == path, result

    # Read-back via an expected-error probe: a second delete must report the
    # asset is gone (AssetNotFound), proving the first delete committed.
    probe = mcp.command("asset_delete", {"asset_path": path, "force": True})
    assert probe.get("status") == "error", probe
    assert not config.uasset_disk_path(path).is_file(), "uasset still on disk after delete"


@pytest.mark.gui_only  # opens a real asset-editor window; fatals the -nullrhi layout-save ticker
@covers("asset_open")
def test_open_asset(mcp, source_bp):
    result = mcp.expect("asset_open", {"asset_path": source_bp})
    assert result["data"]["asset_path"] == source_bp, result
    assert "Blueprint" in result["data"]["asset_class"], result
    # Opening an asset editor is a known crash-prone path; confirm interactivity.
    assert_ready(mcp)


@covers("editor_content_browser_refresh")
def test_content_browser_refresh(mcp):
    result = mcp.expect("editor_content_browser_refresh", {
        "path": "/Game/__MCPTest__",
        "force_rescan": True,
    })
    assert result.get("success") is True, result
    assert result["path"] == "/Game/__MCPTest__", result


@covers("asset_fixup_redirectors")
def test_fixup_redirectors(mcp):
    """A save-then-rename leaves a redirector at the old path; fixup scans the
    namespace for them. Run as a non-destructive dry_run and assert the scan
    reported a result (the empty-namespace case is also a valid success)."""
    src = _make_saved_bp(mcp, f"{NS}/BP_RedirSrc")
    dest = f"{NS}/BP_RedirDst"
    ensure_absent(mcp, dest)
    mcp.expect("asset_rename", {"source_path": src, "new_name": "BP_RedirDst"})
    # Persist both the renamed asset and the redirector left at the old path.
    mcp.expect("asset_save", {"asset_paths": [dest, src]})

    result = mcp.expect("asset_fixup_redirectors", {"directory_path": NS, "dry_run": True})
    assert isinstance(result, dict) and result, result
    # redirectors_found is surfaced on both the empty and non-empty paths.
    found = result.get("redirectors_found", result.get("data", {}).get("redirectors_found"))
    assert found is not None, result


@covers("asset_textures_import")
def test_import_textures(mcp):
    """Write a tiny PNG to disk and import it as a UTexture2D. The handler saves
    the asset, so the uasset must land at the mapped Content path."""
    dest_folder = f"{NS}/tex"
    asset_name = "T_MCPTest"
    tmp = Path(tempfile.gettempdir()) / "mcp_test_import.png"
    tmp.write_bytes(_png_bytes())
    try:
        result = mcp.expect("asset_textures_import", {
            "destination_folder": dest_folder,
            "images": [{"path": str(tmp), "name": asset_name}],
            "force_overwrite": True,
        }, timeout=120.0)
    finally:
        try:
            tmp.unlink()
        except OSError:
            pass

    assert result["count"] >= 1, result
    assert not result.get("failed"), result["failed"]
    imported_path = result["imported"][0]["asset_path"]
    assert asset_name in imported_path, result
    assert config.uasset_disk_path(imported_path).is_file(), "imported texture uasset missing"
