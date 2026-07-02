"""Environment + path resolution for the UnrealMCP integration harness.

All engine/project coupling is funnelled through here so the rest of the
harness stays platform- and install-agnostic. The two env vars below mirror the
ones the production server already documents (README.md), so a machine set up to
run the MCP server is already set up to run these tests:

    UNREAL_ENGINE_ROOT   engine install root (the dir containing Engine/)   [required]
    UE_MCP_TEST_PROJECT  override for the host project dir                  [optional]
                         (default: tests/fixtures/TestProject)

Optional knobs:

    UE_MCP_BRIDGE_HOST   default 127.0.0.1
    UE_MCP_BRIDGE_PORT   default 55557   (must match the C++ plugin's MCP_SERVER_PORT)
    UE_MCP_BOOT_TIMEOUT  seconds to wait for the editor to become interactive (default 420)
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

# tests/harness/config.py -> repo root is three parents up.
REPO_ROOT = Path(__file__).resolve().parents[2]
PLUGIN_SRC = REPO_ROOT / "src" / "Plugin" / "UnrealMCP"
DEFAULT_PROJECT = REPO_ROOT / "tests" / "fixtures" / "TestProject"

# Engine install root comes from UNREAL_ENGINE_ROOT (no hardcoded default —
# see engine_root()).

BRIDGE_HOST = os.environ.get("UE_MCP_BRIDGE_HOST", "127.0.0.1")
BRIDGE_PORT = int(os.environ.get("UE_MCP_BRIDGE_PORT", "55557"))
BOOT_TIMEOUT_S = float(os.environ.get("UE_MCP_BOOT_TIMEOUT", "420"))

# Namespace every test-created on-disk asset lives under, both as a content path
# (/Game/__MCPTest__/...) and the matching Content/ subdir. Deleting the dir is a
# full on-disk reset; it is gitignored in the fixture project.
TEST_CONTENT_NAMESPACE = "__MCPTest__"


def _is_windows() -> bool:
    return sys.platform.startswith("win")


def _is_mac() -> bool:
    return sys.platform == "darwin"


def engine_root() -> Path:
    root = os.environ.get("UNREAL_ENGINE_ROOT")
    if not root:
        raise RuntimeError(
            "UNREAL_ENGINE_ROOT is not set. Point it at your Unreal Engine install "
            "root — the directory containing 'Engine/' "
            r"(e.g. C:\path\to\UnrealEngine or /opt/UnrealEngine)."
        )
    p = Path(root)
    if not (p / "Engine").is_dir():
        raise RuntimeError(
            f"UNREAL_ENGINE_ROOT={p} does not look like an engine root "
            "(no 'Engine/' subdirectory)."
        )
    return p


def _engine_binaries_dir() -> Path:
    if _is_windows():
        plat = "Win64"
    elif _is_mac():
        plat = "Mac"
    else:
        plat = "Linux"
    return engine_root() / "Engine" / "Binaries" / plat


def editor_gui_exe() -> Path:
    """Full editor binary (real RHI + window) — used for render/screenshot tests."""
    name = "UnrealEditor.exe" if _is_windows() else "UnrealEditor"
    return _engine_binaries_dir() / name


def editor_cmd_exe() -> Path:
    """Console editor binary — used for headless (-nullrhi) runs. Falls back to the
    GUI binary on platforms that don't ship a -Cmd variant."""
    if _is_windows():
        return _engine_binaries_dir() / "UnrealEditor-Cmd.exe"
    cmd = _engine_binaries_dir() / "UnrealEditor-Cmd"
    return cmd if cmd.exists() else editor_gui_exe()


def build_script() -> Path:
    bf = engine_root() / "Engine" / "Build" / "BatchFiles"
    if _is_windows():
        return bf / "Build.bat"
    if _is_mac():
        return bf / "Mac" / "Build.sh"
    return bf / "Linux" / "Build.sh"


def project_dir() -> Path:
    return Path(os.environ.get("UE_MCP_TEST_PROJECT", str(DEFAULT_PROJECT))).resolve()


def uproject_path() -> Path:
    d = project_dir()
    matches = sorted(d.glob("*.uproject"))
    if not matches:
        raise RuntimeError(f"No .uproject found in {d}")
    return matches[0]


def project_name() -> str:
    return uproject_path().stem


def editor_target() -> str:
    return f"{project_name()}Editor"


def editor_module_dll() -> Path:
    """The compiled editor module for the project — its presence is the cheap
    'is this project built?' signal the launcher uses to decide auto-build."""
    plat = "Win64" if _is_windows() else ("Mac" if _is_mac() else "Linux")
    ext = ".dll" if _is_windows() else (".dylib" if _is_mac() else ".so")
    return project_dir() / "Binaries" / plat / f"UnrealEditor-{project_name()}{ext}"


def plugin_dest() -> Path:
    """Where the UnrealMCP plugin must live for the editor to load it."""
    return project_dir() / "Plugins" / "UnrealMCP"


def test_content_dir() -> Path:
    return project_dir() / "Content" / TEST_CONTENT_NAMESPACE


def uasset_disk_path(game_path: str, ext: str = ".uasset") -> Path:
    """Map a /Game/... content path to its on-disk package file.

    /Game/Foo/Bar           -> <project>/Content/Foo/Bar.uasset
    /Game/Foo/Bar.Bar       -> <project>/Content/Foo/Bar.uasset  (object suffix dropped)
    Use ext='.umap' for levels. The file only exists after the asset is SAVED.
    """
    pkg = game_path.split(".")[0]
    if not pkg.startswith("/Game/"):
        raise ValueError(f"not a /Game/ path: {game_path}")
    rel = pkg[len("/Game/"):]
    return project_dir() / "Content" / (rel + ext)
