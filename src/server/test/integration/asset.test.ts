/**
 * Asset domain — create assets under the test namespace, then refactor them
 * through the AssetManager bridge (duplicate / rename / move / delete), open them,
 * walk their reference graph, fix up redirectors, rescan the content browser, and
 * import a texture from disk. Port of tests/integration/test_asset.py.
 *
 * Source assets are real on-disk packages under /Game/__MCPTest__/asset/...; the
 * session-end disk wipe cleans them up. Creates are made re-runnable with
 * ensureAbsent before each create.
 *
 * Note on envelope shape: the AssetManager handlers wrap their payload under a
 * `data` sub-object (CreateSuccessResponse), whereas asset_references /
 * content_browser_refresh / import_textures return their fields at the top level.
 */

import { test, expect, beforeAll } from "bun:test";
import { editorSuite, NS as ROOT, GUI } from "../harness/suite.ts";
import { ensureAbsent, payload, assertReady , isFalsyOrEmpty } from "../harness/ops.ts";
import type { Commandable } from "../harness/ops.ts";
import { projectDir } from "../harness/env.ts";
import { existsSync, statSync, writeFileSync, unlinkSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { deflateSync } from "node:zlib";

const NS = `${ROOT}/asset`;

const isFile = (p: string): boolean => existsSync(p) && statSync(p).isFile();

/** Map a /Game/... content path to its on-disk package file (port of
 *  config.uasset_disk_path). */
function uassetDiskPath(gamePath: string, ext = ".uasset"): string {
  const pkg = gamePath.split(".")[0] ?? "";
  if (!pkg.startsWith("/Game/")) throw new Error(`not a /Game/ path: ${gamePath}`);
  const rel = pkg.slice("/Game/".length);
  return join(projectDir(), "Content", rel + ext);
}

/** Idempotently create + save an Actor Blueprint so it exists in the asset
 *  registry and on disk (a prerequisite for the refactor ops). */
async function makeSavedBp(bridge: Commandable, path: string): Promise<string> {
  await ensureAbsent(bridge, path);
  await bridge.expect("bp_create_blueprint", { name: path, parent_class: "Actor" });
  await bridge.expect("asset_save", { asset_paths: [path] });
  return path;
}

/** crc32 (PNG / zlib polynomial), returned as an unsigned 32-bit int. */
function crc32(buf: Uint8Array): number {
  let crc = 0xffffffff;
  for (let i = 0; i < buf.length; i++) {
    crc ^= buf[i]!;
    for (let j = 0; j < 8; j++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

/** A minimal valid 8-bit RGB PNG built with the stdlib (port of _png_bytes). */
function pngBytes(width = 4, height = 4, rgb: [number, number, number] = [200, 120, 60]): Buffer {
  const chunk = (tag: string, data: Buffer): Buffer => {
    const body = Buffer.concat([Buffer.from(tag, "ascii"), data]);
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length, 0);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(body), 0);
    return Buffer.concat([len, body, crc]);
  };
  const sig = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr.writeUInt8(8, 8); // bit depth 8
  ihdr.writeUInt8(2, 9); // color type 2 (RGB)
  ihdr.writeUInt8(0, 10);
  ihdr.writeUInt8(0, 11);
  ihdr.writeUInt8(0, 12);
  const pixel = Buffer.from(rgb);
  const row = Buffer.concat([Buffer.from([0]), ...Array(width).fill(pixel)]); // filter byte 0 + scanline
  const raw = Buffer.concat(Array(height).fill(row));
  const idat = deflateSync(raw);
  return Buffer.concat([sig, chunk("IHDR", ihdr), chunk("IDAT", idat), chunk("IEND", Buffer.alloc(0))]);
}

editorSuite("asset", (ctx) => {
  // One shared, saved source Blueprint for the read-only / copy tests.
  let sourceBp: string;

  beforeAll(async () => {
    sourceBp = await makeSavedBp(ctx.mcp, `${NS}/BP_Source`);
  });

  test("test_duplicate_asset_writes_uasset", async () => {
    const dest = `${NS}/BP_Dup`;
    await ensureAbsent(ctx.mcp, dest);
    const result = await ctx.mcp.expect("asset_duplicate", {
      source_path: sourceBp,
      destination_path: dest,
    });
    expect(result.success).toEqual(true);
    expect(payload(result).destination_path).toEqual(dest);

    // Save the copy, then the package file must exist at the mapped Content path.
    await ctx.mcp.expect("asset_save", { asset_paths: [dest] });
    const disk = uassetDiskPath(dest);
    expect(isFile(disk)).toBe(true);
  });

  test("test_duplicate_asset_dry_run_does_not_create", async () => {
    const dest = `${NS}/BP_DupDry`;
    await ensureAbsent(ctx.mcp, dest);
    const result = await ctx.mcp.expect("asset_duplicate", {
      source_path: sourceBp,
      destination_path: dest,
      dry_run: true,
    });
    expect(result.dry_run).toEqual(true);
    expect((result.diff as { created: { path: string }[] }).created[0]!.path).toEqual(dest);
    expect(existsSync(uassetDiskPath(dest))).toBe(false);
  });

  test("test_rename_asset", async () => {
    const src = await makeSavedBp(ctx.mcp, `${NS}/BP_RenameSrc`);
    const newName = "BP_RenameDst";
    const dest = `${NS}/${newName}`;
    await ensureAbsent(ctx.mcp, dest);

    const result = await ctx.mcp.expect("asset_rename", { source_path: src, new_name: newName });
    expect(result.success).toEqual(true);
    expect(payload(result).new_path).toEqual(dest);

    await ctx.mcp.expect("asset_save", { asset_paths: [dest] });
    expect(isFile(uassetDiskPath(dest))).toBe(true);
  });

  test("test_move_asset", async () => {
    const src = await makeSavedBp(ctx.mcp, `${NS}/BP_MoveSrc`);
    const destFolder = `${NS}/moved`;
    const dest = `${destFolder}/BP_MoveSrc`;
    await ensureAbsent(ctx.mcp, dest);

    const result = await ctx.mcp.expect("asset_move", {
      source_path: src,
      destination_folder: destFolder,
    });
    expect(result.success).toEqual(true);
    expect(payload(result).new_path).toEqual(dest);

    await ctx.mcp.expect("asset_save", { asset_paths: [dest] });
    expect(isFile(uassetDiskPath(dest))).toBe(true);
  });

  test("test_asset_references", async () => {
    // Outbound dependency lookup over the asset reference graph (read-only).
    const result = await ctx.mcp.expect("asset_references", {
      asset_path: sourceBp,
      direction: "outbound",
      depth: 1,
    });
    expect(result.direction).toEqual("outbound");
    const refs = result.references;
    expect(Array.isArray(refs)).toBe(true);
    expect(result.returned_count).toEqual((refs as unknown[]).length);
  });

  test("test_delete_asset_then_absent", async () => {
    const path = await makeSavedBp(ctx.mcp, `${NS}/BP_DelSrc`);

    const result = await ctx.mcp.expect("asset_delete", { asset_path: path, force: true });
    expect(result.success).toEqual(true);
    expect(payload(result).deleted_path).toEqual(path);

    // Read-back via an expected-error probe: a second delete must report the
    // asset is gone (AssetNotFound), proving the first delete committed.
    const probe = await ctx.mcp.command("asset_delete", { asset_path: path, force: true });
    expect(probe.status).toEqual("error");
    expect(isFile(uassetDiskPath(path))).toBe(false);
  });

  // opens a real asset-editor window; fatals the -nullrhi layout-save ticker
  test.skipIf(!GUI)("test_open_asset", async () => {
    const result = await ctx.mcp.expect("asset_open", { asset_path: sourceBp });
    expect(payload(result).asset_path).toEqual(sourceBp);
    expect(String(payload(result).asset_class)).toContain("Blueprint");
    // Opening an asset editor is a known crash-prone path; confirm interactivity.
    await assertReady(ctx.mcp);
  });

  test("test_content_browser_refresh", async () => {
    const result = await ctx.mcp.expect("editor_content_browser_refresh", {
      path: "/Game/__MCPTest__",
      force_rescan: true,
    });
    expect(result.success).toEqual(true);
    expect(result.path).toEqual("/Game/__MCPTest__");
  });

  test("test_fixup_redirectors", async () => {
    // A save-then-rename leaves a redirector at the old path; fixup scans the
    // namespace for them. Run as a non-destructive dry_run and assert the scan
    // reported a result (the empty-namespace case is also a valid success).
    const src = await makeSavedBp(ctx.mcp, `${NS}/BP_RedirSrc`);
    const dest = `${NS}/BP_RedirDst`;
    await ensureAbsent(ctx.mcp, dest);
    await ctx.mcp.expect("asset_rename", { source_path: src, new_name: "BP_RedirDst" });
    // Persist both the renamed asset and the redirector left at the old path.
    await ctx.mcp.expect("asset_save", { asset_paths: [dest, src] });

    const result = await ctx.mcp.expect("asset_fixup_redirectors", {
      directory_path: NS,
      dry_run: true,
    });
    expect(result && typeof result === "object" && Object.keys(result).length > 0).toBeTruthy();
    // redirectors_found is surfaced on both the empty and non-empty paths.
    const found =
      result.redirectors_found ?? (result.data as { redirectors_found?: unknown } | undefined)?.redirectors_found;
    expect(found ?? null).not.toBeNull();
  });

  test("test_import_textures", async () => {
    // Write a tiny PNG to disk and import it as a UTexture2D. The handler saves
    // the asset, so the uasset must land at the mapped Content path.
    const destFolder = `${NS}/tex`;
    const assetName = "T_MCPTest";
    const tmp = join(tmpdir(), "mcp_test_import.png");
    writeFileSync(tmp, pngBytes());
    let result: Record<string, unknown>;
    try {
      result = await ctx.mcp.expect("asset_textures_import", {
        destination_folder: destFolder,
        images: [{ path: tmp, name: assetName }],
        force_overwrite: true,
      });
    } finally {
      try {
        unlinkSync(tmp);
      } catch {
        /* ignore */
      }
    }

    expect((result.count as number) >= 1).toBe(true);
    expect(isFalsyOrEmpty(result.failed)).toBe(true);
    const importedPath = (result.imported as { asset_path: string }[])[0]!.asset_path;
    expect(String(importedPath)).toContain(assetName);
    expect(isFile(uassetDiskPath(importedPath))).toBe(true);
  });
});
