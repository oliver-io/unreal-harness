/**
 * Render test — only runs under GUI (real RHI + window). Port of
 * tests/integration/test_screenshot.py. Demonstrates the second tier of the
 * harness: ops that genuinely need pixels. Under headless (-nullrhi) Slate has
 * no drawable surface and this would fail with window_not_found / engine_busy,
 * so these are skipped unless a GUI editor is running.
 */

import { existsSync, statSync } from "node:fs";

import { test, expect } from "bun:test";
import { editorSuite, GUI } from "../harness/suite.ts";

editorSuite("screenshot", (ctx) => {
  test.skipIf(!GUI)("editor_window_screenshot_writes_png", async () => {
    // Put something in the scene first so the capture isn't an empty viewport.
    await ctx.mcp.expect("actor_spawn", {
      class_path: "/Script/Engine.PointLight",
      location: { x: 0.0, y: 0.0, z: 200.0 },
    });

    const result = await ctx.mcp.expect("editor_window_screenshot", { tab_name: "LevelEditor" });

    const path = result.file_path as string;
    expect(result.bytes).toBeGreaterThan(0);
    expect(existsSync(path)).toBe(true);
    expect(statSync(path).size).toEqual(result.bytes as number);
  });

  test.skipIf(!GUI)("take_screenshot_returns_path", async () => {
    // take_screenshot routes through the editor screenshot request; the file is
    // written asynchronously, so assert on the returned target path, not the file.
    const result = await ctx.mcp.expect("editor_screenshot", { mode: "editor" });
    const blob = JSON.stringify(result).toLowerCase();
    expect(blob.includes(".png") || blob.includes("path")).toBeTruthy();
  });
});
