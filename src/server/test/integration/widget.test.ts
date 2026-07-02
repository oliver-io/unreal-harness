/**
 * UMG Widget domain — create a Widget Blueprint, build its widget tree, mutate a
 * widget property, read the tree back, and bind an event handler. Port of
 * tests/integration/test_widget.py.
 *
 * Self-contained: needs no imported content. A freshly created UWidgetBlueprint
 * has an empty WidgetTree, so the first `widget_add_child` (no parent_name)
 * installs the root panel; a second call parents a Button under it.
 *
 * The whole module is GUI-only: the UMG authoring ops touch the widget editor;
 * under -nullrhi that schedules a deferred Slate layout-save which fatals on the
 * headless generic window.
 */

import { test, expect, beforeAll } from "bun:test";
import { existsSync, statSync } from "node:fs";
import { join } from "node:path";
import { editorSuite, NS as ROOT, GUI } from "../harness/suite.ts";
import { ensureAbsent, assertReady } from "../harness/ops.ts";
import { projectDir } from "../harness/env.ts";

const NS = `${ROOT}/widget`;
const SAMPLE = `${NS}/WBP_Sample`;
const ROOT_NAME = "RootCanvas";
const BUTTON = "MCPTestButton";

/** Map a /Game/... content path to its on-disk package file. The file only
 *  exists after the asset is SAVED. Mirrors config.uasset_disk_path. */
function uassetDiskPath(gamePath: string, ext = ".uasset"): string {
  const pkg = gamePath.split(".")[0] ?? "";
  if (!pkg.startsWith("/Game/")) throw new Error(`not a /Game/ path: ${gamePath}`);
  const rel = pkg.slice("/Game/".length);
  return join(projectDir(), "Content", rel + ext);
}

editorSuite("widget", (ctx) => {
  // module-scoped sample_widget fixture: a WidgetBlueprint with a CanvasPanel
  // root + a Button child. Only built in GUI mode (whole module is GUI-only).
  const sample: { path: string; root: string; button: string } = {
    path: SAMPLE,
    root: ROOT_NAME,
    button: BUTTON,
  };

  beforeAll(async () => {
    if (!GUI) return;
    await ensureAbsent(ctx.mcp, SAMPLE);
    await ctx.mcp.expect("widget_create", { asset_path: SAMPLE });
    // Empty tree -> this child becomes the root (parent_name omitted).
    const root = await ctx.mcp.expect("widget_add_child", {
      widget_path: SAMPLE,
      child_class: "/Script/UMG.CanvasPanel",
      child_name: ROOT_NAME,
    });
    const rootName = (root.child_name as string) ?? ROOT_NAME;
    const child = await ctx.mcp.expect("widget_add_child", {
      widget_path: SAMPLE,
      child_class: "/Script/UMG.Button",
      parent_name: rootName,
      child_name: BUTTON,
    });
    sample.root = rootName;
    sample.button = (child.child_name as string) ?? BUTTON;
  });

  test.skipIf(!GUI)("test_widget_create_writes_uasset_on_disk", async () => {
    const path = `${NS}/WBP_Created`;
    await ensureAbsent(ctx.mcp, path);
    const result = await ctx.mcp.expect("widget_create", { asset_path: path });
    expect(result.success).toBe(true);
    // widget_create auto-saves on success, so the package must exist on disk.
    const disk = uassetDiskPath(path);
    expect(existsSync(disk) && statSync(disk).isFile()).toBe(true);
  });

  test.skipIf(!GUI)("test_widget_tree_read_lists_child", async () => {
    const result = await ctx.mcp.expect("widget_tree_read", { widget_path: sample.path });
    const blob = JSON.stringify(result);
    expect(blob).toContain(sample.root);
    expect(blob).toContain(sample.button);
    // The button's class should surface in the dumped tree.
    expect(blob).toContain("Button");
  });

  test.skipIf(!GUI)("test_widget_set_property_then_readback", async () => {
    const result = await ctx.mcp.expect("widget_set_property", {
      widget_path: sample.path,
      widget_name: sample.button,
      property_name: "IsEnabled",
      property_value: false,
      target: "widget",
    });
    expect(result.success).not.toBe(false);
    expect(result.property_name).toEqual("IsEnabled");
    // The setter captures before/after exported text; 'after' must reflect False.
    expect(result.after).toBeDefined();
    expect(String(result.after ?? "").toLowerCase()).toContain("false");
  });

  test.skipIf(!GUI)("test_widget_bind_handler", async () => {
    // widget_add_child already recompiled the WBP, so the Button is present on the
    // generated class as required by the bind path.
    const result = await ctx.mcp.expect("widget_bind_handler", {
      widget_path: sample.path,
      widget_name: sample.button,
      event_name: "OnClicked",
    });
    expect(result.success).not.toBe(false);
    expect(result.event_name).toEqual("OnClicked");
    await assertReady(ctx.mcp);
  });
});
