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
    // Manufacture a REAL redirector, then assert the fixup scan reports THAT
    // redirector. A plain save+rename leaves NO redirector in UE 5.7 (verified
    // live — the old "found is defined" assertion passed on found == 0).
    // Deterministic manufacture: put the asset in a temporary LOCAL collection
    // first — FAssetRenameManager::DetectReferencingCollections forces
    // bCreateRedirector for collection-referenced assets. Collection arrange /
    // teardown uses the sanctioned py escape hatch (no typed primitive).
    const parent = `${NS}/M_RedirParent`;
    const renamed = `${NS}/M_RedirParentRenamed`;
    const col = "MCPTestRedirCol";
    for (const p of [parent, renamed]) await ensureAbsent(ctx.mcp, p);
    await ctx.mcp.expect("material_create", { material_path: parent });
    await ctx.mcp.expect("asset_save", { asset_paths: [parent] });
    try {
      const probe = await ctx.mcp.expect("editor_console_exec", {
        command:
          "py import unreal; " +
          `a = unreal.load_asset('${parent}'); ` +
          "ats = unreal.get_engine_subsystem(unreal.AssetTagsSubsystem); " +
          `ats.create_collection(unreal.Name('${col}'), unreal.CollectionShareType.LOCAL); ` +
          `print('MCPCOL ADDED=' + str(ats.add_asset_ptr_to_collection(unreal.Name('${col}'), a)))`,
      });
      expect(String(probe.output)).toContain("MCPCOL ADDED=True");

      await ctx.mcp.expect("asset_rename", {
        source_path: parent,
        new_name: "M_RedirParentRenamed",
      });

      const result = await ctx.mcp.expect("asset_fixup_redirectors", {
        directory_path: NS,
        dry_run: true,
      });
      expect(result.dry_run).toBe(true);
      const found =
        result.redirectors_found ?? (result.data as { redirectors_found?: unknown } | undefined)?.redirectors_found;
      expect(Number(found)).toBeGreaterThanOrEqual(1);
      // The manufactured redirector at the OLD parent path is in the diff.
      const deleted = (result.diff as { deleted: { path: string }[] }).deleted;
      const deletedPkgs = deleted.map((d) => d.path.split(".")[0]);
      expect(deletedPkgs).toContain(parent);
    } finally {
      await ctx.mcp.command("editor_console_exec", {
        command:
          "py import unreal; " +
          "ats = unreal.get_engine_subsystem(unreal.AssetTagsSubsystem); " +
          `ats.destroy_collection(unreal.Name('${col}'))`,
      });
      // ensureAbsent(parent) also deletes the manufactured redirector.
      for (const p of [parent, renamed]) await ensureAbsent(ctx.mcp, p);
    }
  });

  // ── importers: mesh / audio / font (generated sources) ────────────────────
  //
  // Parity twins of the pytest importer tests: generate the source file (OBJ /
  // WAV) or use the engine's own Slate TTF, import, and observe through
  // INDEPENDENT reads — asset_list class, a geometry/duration readback, and
  // the saved package on disk. The disk root comes from the LIVE editor
  // (project_context), so the assertion holds in attach mode even when the
  // attached project differs from UE_MCP_TEST_PROJECT.

  /** Attach-safe /Game/... -> Content/....uasset mapping via the live editor's
   *  own project root (project_context.settings_paths[0] = FPaths::ProjectDir()). */
  async function liveUassetDiskPath(gamePath: string): Promise<string> {
    const pctx = await ctx.mcp.expect("project_context", {});
    const root = (pctx.settings_paths as string[])[0]!;
    const pkg = gamePath.split(".")[0]!;
    if (!pkg.startsWith("/Game/")) throw new Error(`not a /Game/ path: ${gamePath}`);
    return join(root, "Content", pkg.slice("/Game/".length) + ".uasset");
  }

  /** {name: class} for the assets directly under a folder, read from the
   *  asset registry (independent of any importer's echo). */
  async function assetClassesIn(folder: string): Promise<Record<string, string>> {
    const listing = payload(
      await ctx.mcp.expect("asset_list", { directory_path: folder, recursive: false }),
    );
    const out: Record<string, string> = {};
    for (const a of listing.assets as { name: string; class: string }[]) out[a.name] = a.class;
    return out;
  }

  /** A minimal OBJ: an axis-aligned cube spanning ±half on every axis
   *  (8 vertices, 12 triangles) — known extents prove real imported geometry. */
  function objCubeText(half = 50): string {
    const v: string[] = [];
    for (const z of [-half, half])
      for (const y of [-half, half]) for (const x of [-half, half]) v.push(`v ${x} ${y} ${z}`);
    // 1..4 = bottom (-z) ring, 5..8 = top (+z) ring, each (-x,-y)(x,-y)(-x,y)(x,y).
    const f = [
      [1, 3, 4], [1, 4, 2], // bottom
      [5, 6, 8], [5, 8, 7], // top
      [1, 2, 6], [1, 6, 5], // -y
      [3, 7, 8], [3, 8, 4], // +y
      [1, 5, 7], [1, 7, 3], // -x
      [2, 4, 8], [2, 8, 6], // +x
    ].map((t) => `f ${t.join(" ")}`);
    return ["# MCP test cube", ...v, ...f].join("\n") + "\n";
  }

  /** A valid 16-bit PCM mono WAV with an EXACT sample count (rate*seconds),
   *  so the imported USoundWave must report exactly `seconds` of duration. */
  function wavBytes(seconds = 0.5, rate = 44100, freq = 440): Buffer {
    const n = Math.round(rate * seconds);
    const buf = Buffer.alloc(44 + n * 2);
    buf.write("RIFF", 0);
    buf.writeUInt32LE(36 + n * 2, 4);
    buf.write("WAVE", 8);
    buf.write("fmt ", 12);
    buf.writeUInt32LE(16, 16); // fmt chunk size
    buf.writeUInt16LE(1, 20); // PCM
    buf.writeUInt16LE(1, 22); // mono
    buf.writeUInt32LE(rate, 24);
    buf.writeUInt32LE(rate * 2, 28); // byte rate
    buf.writeUInt16LE(2, 32); // block align
    buf.writeUInt16LE(16, 34); // bits per sample
    buf.write("data", 36);
    buf.writeUInt32LE(n * 2, 40);
    for (let i = 0; i < n; i++)
      buf.writeInt16LE(Math.round(8000 * Math.sin((2 * Math.PI * freq * i) / rate)), 44 + i * 2);
    return buf;
  }

  test(
    "test_import_mesh_obj_cube_reports_known_bounds",
    async () => {
      // Generate an OBJ cube spanning ±50, import it (UFbxFactory handles
      // .obj), then observe: asset_list sees a StaticMesh, mesh_get_bounds
      // reports the known extents, and the saved .uasset exists on disk.
      const destFolder = `${NS}/imported`;
      const assetName = "SM_MCPTestCube";
      const assetPath = `${destFolder}/${assetName}`;
      await ensureAbsent(ctx.mcp, assetPath);
      const tmp = join(tmpdir(), "mcp_test_import_cube.obj");
      writeFileSync(tmp, objCubeText(50));
      try {
        const result = await ctx.mcp.expect("asset_import_mesh", {
          source_path: tmp,
          destination_folder: destFolder,
          name: assetName,
          import_materials: false,
          import_textures: false,
        });
        expect(result.count).toEqual(1);
        expect(isFalsyOrEmpty(result.failed)).toBe(true);
        expect((result.imported as { class: string }[])[0]!.class).toEqual("StaticMesh");

        // Observer 1: the asset registry sees a StaticMesh at the destination.
        expect((await assetClassesIn(destFolder))[assetName]).toEqual("StaticMesh");

        // Observer 2: geometry — a ±50 cube reports extent 50 / size 100 per
        // axis, centered at the origin.
        const bounds = await ctx.mcp.expect("mesh_get_bounds", { static_mesh_path: assetPath });
        const lb = bounds.local_bounds as {
          box_extent: Record<string, number>;
          origin: Record<string, number>;
        };
        const size = bounds.size as Record<string, number>;
        for (const axis of ["x", "y", "z"]) {
          expect(Math.abs(lb.box_extent[axis]! - 50)).toBeLessThan(0.1);
          expect(Math.abs(lb.origin[axis]!)).toBeLessThan(0.1);
          expect(Math.abs(size[axis]! - 100)).toBeLessThan(0.1);
        }

        // Observer 3: the importer saves — the package must be on disk.
        expect(isFile(await liveUassetDiskPath(assetPath))).toBe(true);
      } finally {
        await ensureAbsent(ctx.mcp, assetPath);
        try {
          unlinkSync(tmp);
        } catch {
          /* ignore */
        }
      }
    },
    180000,
  );

  test(
    "test_import_audio_wav_duration_readback",
    async () => {
      // Generate exactly 0.5 s of 16-bit PCM, import with looping=true, and
      // read duration + looping back off the live USoundWave. No typed read
      // primitive surfaces USoundWave::Duration, so the independent observer
      // is the sanctioned `py` console escape hatch (editor_console_exec), on
      // top of the registry class check and the on-disk package.
      const destFolder = `${NS}/imported`;
      const assetName = "S_MCPTestTone";
      const assetPath = `${destFolder}/${assetName}`;
      await ensureAbsent(ctx.mcp, assetPath);
      const tmp = join(tmpdir(), "mcp_test_import_tone.wav");
      writeFileSync(tmp, wavBytes(0.5, 44100));
      try {
        const result = await ctx.mcp.expect("asset_import_audio", {
          destination_folder: destFolder,
          sounds: [{ path: tmp, name: assetName, looping: true }],
        });
        expect(result.count).toEqual(1);
        expect(isFalsyOrEmpty(result.failed)).toBe(true);
        expect((result.imported as { looping: boolean }[])[0]!.looping).toBe(true);

        // Observer 1: the asset registry sees a SoundWave at the destination.
        expect((await assetClassesIn(destFolder))[assetName]).toEqual("SoundWave");

        // Observer 2: duration + looping read off the loaded asset itself.
        const probe = await ctx.mcp.expect("editor_console_exec", {
          command:
            `py import unreal; w = unreal.load_asset('${assetPath}'); ` +
            `print('MCPWAV DUR=%.4f LOOP=%s' % (w.get_editor_property('duration'), ` +
            `w.get_editor_property('looping')))`,
        });
        const m = /MCPWAV DUR=([\d.]+) LOOP=(\w+)/.exec(String(probe.output));
        expect(m).not.toBeNull();
        expect(Math.abs(Number(m![1]) - 0.5)).toBeLessThan(0.005);
        expect(m![2]).toEqual("True");

        // Observer 3: the importer saves — the package must be on disk.
        expect(isFile(await liveUassetDiskPath(assetPath))).toBe(true);
      } finally {
        await ensureAbsent(ctx.mcp, assetPath);
        try {
          unlinkSync(tmp);
        } catch {
          /* ignore */
        }
      }
    },
    180000,
  );

  // Source TTF: the engine's own Slate font (ships with every UE install) —
  // chosen over Windows fonts for portability. Skipped when the env doesn't
  // locate an engine (mirrors the pytest skip).
  const engineFont = process.env.UNREAL_ENGINE_ROOT
    ? join(process.env.UNREAL_ENGINE_ROOT, "Engine", "Content", "Slate", "Fonts", "Roboto-Regular.ttf")
    : null;

  test.skipIf(!engineFont || !existsSync(engineFont))(
    "test_import_font_ttf_creates_font_and_face",
    async () => {
      // Import a TTF as a runtime UFont + backing UFontFace; observe via the
      // asset registry (Font + FontFace classes) and both packages on disk.
      const destFolder = `${NS}/imported`;
      const assetName = "F_MCPTestFont";
      const fontPath = `${destFolder}/${assetName}`;
      const facePath = `${destFolder}/${assetName}_Face`;
      await ensureAbsent(ctx.mcp, fontPath);
      await ensureAbsent(ctx.mcp, facePath);
      try {
        const result = await ctx.mcp.expect("asset_import_font", {
          destination_folder: destFolder,
          fonts: [{ path: engineFont, name: assetName }],
        });
        expect(result.count).toEqual(1);
        expect(isFalsyOrEmpty(result.failed)).toBe(true);
        const entry = (result.imported as { typeface: string; face_path: string }[])[0]!;
        expect(entry.typeface).toEqual("Regular");
        expect(entry.face_path.split(".")[0]).toEqual(facePath);

        // Observer 1: the registry sees the runtime UFont AND its UFontFace.
        const classes = await assetClassesIn(destFolder);
        expect(classes[assetName]).toEqual("Font");
        expect(classes[`${assetName}_Face`]).toEqual("FontFace");

        // Observer 2: both saved packages on disk.
        expect(isFile(await liveUassetDiskPath(fontPath))).toBe(true);
        expect(isFile(await liveUassetDiskPath(facePath))).toBe(true);
      } finally {
        await ensureAbsent(ctx.mcp, fontPath);
        await ensureAbsent(ctx.mcp, facePath);
      }
    },
    180000,
  );

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
