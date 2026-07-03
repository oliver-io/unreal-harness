/**
 * Render test — only runs under GUI (real RHI + window). Port of
 * tests/integration/test_screenshot.py. Demonstrates the second tier of the
 * harness: ops that genuinely need pixels. Under headless (-nullrhi) Slate has
 * no drawable surface and this would fail with window_not_found / engine_busy,
 * so these are skipped unless a GUI editor is running.
 */

import { existsSync, statSync, unlinkSync } from "node:fs";

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

  test.skipIf(!GUI)("take_screenshot_writes_png", async () => {
    // editor_screenshot must produce a real PNG on disk, not just echo a target
    // path. mode=editor under GUI captures synchronously; the async fallback is
    // confirmed by the bridge before it returns (GAP-007) — either way the file
    // must exist. Poll briefly for filesystem visibility, then clean up the
    // capture (self-cleaning: the filename is namespaced to this run).
    const result = await ctx.mcp.expect("editor_screenshot", {
      mode: "editor",
      filename: `MCPTest_Screenshot_${Date.now()}`,
    });
    const path = result.file_path as string;
    expect(path.toLowerCase().endsWith(".png")).toBe(true);
    expect(["captured", "requested"]).toContain(result.status as string);
    try {
      const deadline = Date.now() + 15_000;
      while (!existsSync(path) && Date.now() < deadline) {
        await new Promise((r) => setTimeout(r, 250));
      }
      expect(existsSync(path)).toBe(true);
      expect(statSync(path).size).toBeGreaterThan(0);
    } finally {
      try {
        unlinkSync(path);
      } catch {
        /* ignore */
      }
    }
  });
});
