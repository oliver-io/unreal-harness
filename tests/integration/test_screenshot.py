"""Render test — only runs under --ue-mode=gui (real RHI + window). Driven
through the real MCP server (the `mcp` fixture calls tools by name).

Demonstrates the second tier of the harness: ops that genuinely need pixels.
Under headless (-nullrhi) Slate has no drawable surface and this would fail with
window_not_found / engine_busy, so it is skipped unless a GUI editor is running.
"""

import os

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
def test_take_screenshot_returns_path(mcp):
    # take_screenshot routes through the editor screenshot request; the file is
    # written asynchronously, so assert on the returned target path, not the file.
    result = mcp.expect("editor_screenshot", {"mode": "editor"})
    blob = str(result).lower()
    assert ".png" in blob or "path" in blob, result
