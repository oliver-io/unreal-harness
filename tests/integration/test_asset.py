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

import math
import re
import struct
import tempfile
import zlib
from pathlib import Path

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready, payload

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
    """Manufacture a REAL redirector, then assert the fixup scan reports THAT
    redirector. A plain save+rename leaves NO redirector in UE 5.7 (verified
    live: with every referencer loaded the rename manager fixes references in
    memory and trashes the old package — the old "found is not None" assertion
    passed on found == 0 and proved nothing). Deterministic manufacture: put
    the asset in a temporary LOCAL collection first —
    FAssetRenameManager::DetectReferencingCollections forces bCreateRedirector
    for collection-referenced assets (verified live). The collection
    arrange/teardown uses the sanctioned py escape hatch (no typed collection
    primitive exists); the rename itself goes through the typed op."""
    parent = f"{NS}/M_RedirParent"
    renamed = f"{NS}/M_RedirParentRenamed"
    col = "MCPTestRedirCol"
    for p in (parent, renamed):
        ensure_absent(mcp, p)
    mcp.expect("material_create", {"material_path": parent})
    mcp.expect("asset_save", {"asset_paths": [parent]})
    try:
        probe = mcp.expect("editor_console_exec", {"command": (
            "py import unreal; "
            f"a = unreal.load_asset('{parent}'); "
            "ats = unreal.get_engine_subsystem(unreal.AssetTagsSubsystem); "
            f"ats.create_collection(unreal.Name('{col}'), unreal.CollectionShareType.LOCAL); "
            f"print('MCPCOL ADDED=' + str(ats.add_asset_ptr_to_collection(unreal.Name('{col}'), a)))"
        )})
        assert "MCPCOL ADDED=True" in probe["output"], probe

        mcp.expect("asset_rename", {
            "source_path": parent,
            "new_name": "M_RedirParentRenamed",
        })

        result = mcp.expect("asset_fixup_redirectors", {"directory_path": NS, "dry_run": True})
        assert result.get("dry_run") is True, result
        found = result.get("redirectors_found", result.get("data", {}).get("redirectors_found"))
        assert found is not None and found >= 1, result
        # The manufactured redirector at the OLD parent path is in the diff.
        deleted_pkgs = {d.get("path", "").split(".")[0] for d in result["diff"]["deleted"]}
        assert parent in deleted_pkgs, (parent, deleted_pkgs)
    finally:
        mcp.command("editor_console_exec", {"command": (
            "py import unreal; "
            "ats = unreal.get_engine_subsystem(unreal.AssetTagsSubsystem); "
            f"ats.destroy_collection(unreal.Name('{col}'))"
        )})
        # ensure_absent(parent) also deletes the manufactured redirector.
        for p in (parent, renamed):
            ensure_absent(mcp, p)


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


# ── importers: mesh / audio / font (generated sources) ──────────────────────
#
# These three write their own source file (OBJ / WAV) or use a known engine
# font (TTF), import it, and observe through INDEPENDENT reads: the asset
# registry (asset_list class), a geometry/duration readback, and the saved
# package on disk. The disk root is derived from the LIVE editor
# (project_context) rather than harness env, so the assertion holds under
# --ue-attach even when the attached editor's project differs from
# UE_MCP_TEST_PROJECT.


def _live_uasset_disk_path(bridge, game_path: str) -> Path:
    """Attach-safe /Game/... -> Content/....uasset mapping using the live
    editor's own project root (project_context.settings_paths[0] is
    FPaths::ProjectDir(), absolute)."""
    ctx = bridge.expect("project_context", {})
    root = Path(ctx["settings_paths"][0])
    pkg = game_path.split(".")[0]
    assert pkg.startswith("/Game/"), game_path
    return root / "Content" / (pkg[len("/Game/"):] + ".uasset")


def _asset_classes_in(bridge, folder: str) -> dict:
    """{name: class} for the assets directly under a /Game/... folder, read
    from the asset registry (independent of any importer's echo)."""
    listing = payload(bridge.expect("asset_list", {
        "directory_path": folder,
        "recursive": False,
    }))
    return {a["name"]: a["class"] for a in listing["assets"]}


def _obj_cube_text(half: float = 50.0) -> str:
    """A minimal OBJ: an axis-aligned cube spanning ±half on every axis
    (8 vertices, 12 triangles) — known extents prove real imported geometry."""
    v = [(x, y, z) for z in (-half, half) for y in (-half, half) for x in (-half, half)]
    # Vertex order above: 1..4 = bottom (-z) ring, 5..8 = top (+z) ring,
    # each ring ordered (-x,-y) (x,-y) (-x,y) (x,y).
    faces = [
        (1, 3, 4), (1, 4, 2),  # bottom
        (5, 6, 8), (5, 8, 7),  # top
        (1, 2, 6), (1, 6, 5),  # -y
        (3, 7, 8), (3, 8, 4),  # +y
        (1, 5, 7), (1, 7, 3),  # -x
        (2, 4, 8), (2, 8, 6),  # +x
    ]
    lines = ["# MCP test cube"]
    lines += [f"v {x} {y} {z}" for (x, y, z) in v]
    lines += [f"f {a} {b} {c}" for (a, b, c) in faces]
    return "\n".join(lines) + "\n"


def _wav_bytes(seconds: float = 0.5, rate: int = 44100, freq: float = 440.0) -> bytes:
    """A valid 16-bit PCM mono WAV with an EXACT sample count (rate*seconds),
    so the imported USoundWave must report exactly `seconds` of duration."""
    n = round(rate * seconds)
    samples = b"".join(
        struct.pack("<h", round(8000 * math.sin(2 * math.pi * freq * i / rate)))
        for i in range(n)
    )
    hdr = (
        b"RIFF" + struct.pack("<I", 36 + len(samples)) + b"WAVE"
        + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
        + b"data" + struct.pack("<I", len(samples))
    )
    return hdr + samples


@covers("asset_import_mesh")
def test_import_mesh_obj_cube_reports_known_bounds(mcp):
    """Generate an OBJ cube spanning ±50, import it (UFbxFactory handles .obj),
    then observe through three independent reads: asset_list sees a StaticMesh,
    mesh_get_bounds reports the known extents (real geometry, not just a
    package), and the saved .uasset exists on disk."""
    dest_folder = f"{NS}/imported"
    asset_name = "SM_MCPTestCube"
    asset_path = f"{dest_folder}/{asset_name}"
    ensure_absent(mcp, asset_path)
    tmp = Path(tempfile.gettempdir()) / "mcp_test_import_cube.obj"
    tmp.write_text(_obj_cube_text(50.0))
    try:
        result = mcp.expect("asset_import_mesh", {
            "source_path": str(tmp),
            "destination_folder": dest_folder,
            "name": asset_name,
            "import_materials": False,
            "import_textures": False,
        }, timeout=180.0)
        assert result["count"] == 1, result
        assert not result.get("failed"), result["failed"]
        assert result["imported"][0]["class"] == "StaticMesh", result

        # Observer 1: the asset registry sees a StaticMesh at the destination.
        assert _asset_classes_in(mcp, dest_folder).get(asset_name) == "StaticMesh"

        # Observer 2: geometry — a ±50 cube must report extent 50 / size 100
        # per axis, centered at the origin.
        bounds = mcp.expect("mesh_get_bounds", {"static_mesh_path": asset_path})
        lb = bounds["local_bounds"]
        for axis in ("x", "y", "z"):
            assert abs(lb["box_extent"][axis] - 50) < 0.1, lb
            assert abs(lb["origin"][axis]) < 0.1, lb
            assert abs(bounds["size"][axis] - 100) < 0.1, bounds

        # Observer 3: the importer saves — the package must be on disk.
        assert _live_uasset_disk_path(mcp, asset_path).is_file(), "imported mesh uasset missing"
    finally:
        ensure_absent(mcp, asset_path)
        try:
            tmp.unlink()
        except OSError:
            pass


@covers("asset_import_audio")
def test_import_audio_wav_duration_readback(mcp):
    """Generate a WAV with exactly 0.5 s of 16-bit PCM, import it with
    looping=true, and read the duration + looping flag back off the live
    USoundWave. No typed read primitive surfaces USoundWave::Duration, so the
    independent observer is the sanctioned `py` console escape hatch
    (editor_console_exec), on top of the asset-registry class check and the
    on-disk package."""
    dest_folder = f"{NS}/imported"
    asset_name = "S_MCPTestTone"
    asset_path = f"{dest_folder}/{asset_name}"
    ensure_absent(mcp, asset_path)
    tmp = Path(tempfile.gettempdir()) / "mcp_test_import_tone.wav"
    tmp.write_bytes(_wav_bytes(seconds=0.5, rate=44100))
    try:
        result = mcp.expect("asset_import_audio", {
            "destination_folder": dest_folder,
            "sounds": [{"path": str(tmp), "name": asset_name, "looping": True}],
        }, timeout=180.0)
        assert result["count"] == 1, result
        assert not result.get("failed"), result["failed"]
        assert result["imported"][0]["looping"] is True, result

        # Observer 1: the asset registry sees a SoundWave at the destination.
        assert _asset_classes_in(mcp, dest_folder).get(asset_name) == "SoundWave"

        # Observer 2: duration + looping read off the loaded asset itself.
        probe = mcp.expect("editor_console_exec", {
            "command": (
                f"py import unreal; w = unreal.load_asset('{asset_path}'); "
                "print('MCPWAV DUR={:.4f} LOOP={}'.format("
                "w.get_editor_property('duration'), "
                "w.get_editor_property('looping')))"
            ),
        })
        m = re.search(r"MCPWAV DUR=([\d.]+) LOOP=(\w+)", probe["output"])
        assert m, probe
        assert abs(float(m.group(1)) - 0.5) < 0.005, probe["output"]
        assert m.group(2) == "True", probe["output"]

        # Observer 3: the importer saves — the package must be on disk.
        assert _live_uasset_disk_path(mcp, asset_path).is_file(), "imported wav uasset missing"
    finally:
        ensure_absent(mcp, asset_path)
        try:
            tmp.unlink()
        except OSError:
            pass


@covers("asset_import_font")
def test_import_font_ttf_creates_font_and_face(mcp):
    """Import a TTF as a runtime UFont + backing UFontFace. Source: the
    engine's own Slate font (Roboto-Regular.ttf ships with every UE install),
    chosen over Windows fonts for portability. Observed via the asset
    registry (Font + FontFace classes at the destination) and both saved
    packages on disk."""
    try:
        src = config.engine_root() / "Engine" / "Content" / "Slate" / "Fonts" / "Roboto-Regular.ttf"
    except RuntimeError:
        pytest.skip("UNREAL_ENGINE_ROOT not set — no portable TTF source available")
    if not src.is_file():
        pytest.skip(f"engine Slate font not found: {src}")

    dest_folder = f"{NS}/imported"
    asset_name = "F_MCPTestFont"
    font_path = f"{dest_folder}/{asset_name}"
    face_path = f"{dest_folder}/{asset_name}_Face"
    ensure_absent(mcp, font_path)
    ensure_absent(mcp, face_path)
    try:
        result = mcp.expect("asset_import_font", {
            "destination_folder": dest_folder,
            "fonts": [{"path": str(src), "name": asset_name}],
        }, timeout=180.0)
        assert result["count"] == 1, result
        assert not result.get("failed"), result["failed"]
        entry = result["imported"][0]
        assert entry["typeface"] == "Regular", result
        assert entry["face_path"].split(".")[0] == face_path, result

        # Observer 1: the registry sees the runtime UFont AND its UFontFace.
        classes = _asset_classes_in(mcp, dest_folder)
        assert classes.get(asset_name) == "Font", classes
        assert classes.get(f"{asset_name}_Face") == "FontFace", classes

        # Observer 2: both saved packages on disk.
        assert _live_uasset_disk_path(mcp, font_path).is_file(), "font uasset missing"
        assert _live_uasset_disk_path(mcp, face_path).is_file(), "font face uasset missing"
    finally:
        ensure_absent(mcp, font_path)
        ensure_absent(mcp, face_path)
