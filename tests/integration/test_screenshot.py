"""Render test — only runs under --ue-mode=gui (real RHI + window). Driven
through the real MCP server (the `mcp` fixture calls tools by name).

Demonstrates the second tier of the harness: ops that genuinely need pixels.
Under headless (-nullrhi) Slate has no drawable surface and this would fail with
window_not_found / engine_busy, so it is skipped unless a GUI editor is running.
"""

import os
import time

import pytest

from harness.coverage import covers


@pytest.mark.render
@covers("editor_window_screenshot")
def test_editor_window_screenshot_writes_png(mcp):
    # Put something in the scene first so the capture isn't an empty viewport.
    mcp.expect("actor_spawn", {
        "class_path": "/Script/Engine.PointLight",
        "location": {"x": 0.0, "y": 0.0, "z": 200.0},
    })

    result = mcp.expect("editor_window_screenshot", {"tab_name": "LevelEditor"})

    path = result["file_path"]
    assert result["bytes"] > 0
    assert os.path.isfile(path), f"screenshot not written to {path}"
    assert os.path.getsize(path) == result["bytes"]


@pytest.mark.render
@covers("editor_screenshot")
def test_take_screenshot_writes_png(mcp):
    """editor_screenshot must produce a real PNG on disk, not just echo a target
    path. mode=editor under GUI captures synchronously; the async editor-viewport
    fallback is confirmed by the bridge before it returns (GAP-007) — either way
    the file must exist. Poll briefly for filesystem visibility, then clean up
    the capture (self-cleaning: the file is namespaced to this test)."""
    result = mcp.expect("editor_screenshot", {
        "mode": "editor",
        "filename": f"MCPTest_Screenshot_{int(time.time())}",
    })
    path = result["file_path"]
    assert path.lower().endswith(".png"), result
    assert result.get("status") in ("captured", "requested"), result
    try:
        deadline = time.monotonic() + 15.0
        while not os.path.isfile(path) and time.monotonic() < deadline:
            time.sleep(0.25)
        assert os.path.isfile(path), f"screenshot not written to {path}: {result}"
        assert os.path.getsize(path) > 0, f"empty screenshot at {path}"
    finally:
        try:
            os.remove(path)
        except OSError:
            pass
