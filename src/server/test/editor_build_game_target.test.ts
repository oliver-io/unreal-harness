/**
 * Guard-slice test for the `editor_build_game_target` TOOL (host-local UBT
 * build). Act: invoke the handler pulled from the canonical registry
 * (`buildRegistry()`), the same ToolDef an MCP client's call is dispatched to.
 *
 * Scope — validation gates ONLY. The handler runs four cheap, real checks
 * before it ever reaches `spawnSync` (project-root resolution, the Unix-path
 * fallback, the exactly-one-`.uproject` scan, the engine `Build.bat` probe);
 * every test here arranges real directories/files on disk and asserts the
 * documented error object each gate returns. No UBT process is ever spawned:
 * the deepest case stops at the Build.bat stat (UNREAL_ENGINE_ROOT unset or
 * pointing at a dir without Engine/Build/BatchFiles/Build.bat), so the shared
 * build lock / toolchain are never touched. The POSITIVE path (a real
 * multi-minute UBT build) is deliberately untested — see the #DEFERRED note in
 * docs/loops/tests/TASKS.md; mocking the spawn would test the mock.
 *
 * Env handling: the handler reads UNREAL_PROJECT_ROOT / UNREAL_ENGINE_ROOT at
 * CALL time (no import-time snapshot), so each test sets process.env directly
 * and afterEach restores the originals — state never bleeds into other test
 * files sharing this bun process.
 */

import { expect, test, describe, beforeEach, afterEach } from "bun:test";
import { mkdtempSync, rmSync, writeFileSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { buildRegistry } from "../src/register.ts";
import type { ToolContext, ToolDef } from "../src/registry/types.ts";
import { covers } from "./harness/coverage.ts";

// The handler never touches the bridge (host-local subprocess tool); a
// connection-less context matches how result_read.test.ts drives it.
const ctx = { conn: undefined } as unknown as ToolContext;

function buildGameTargetTool(): ToolDef {
  const def = buildRegistry().get("editor_build_game_target");
  if (!def)
    throw new Error("editor_build_game_target not registered in the canonical registry");
  return def;
}

const savedProjectRoot = process.env.UNREAL_PROJECT_ROOT;
const savedEngineRoot = process.env.UNREAL_ENGINE_ROOT;
const scratchDirs: string[] = [];

/** Real temp dir the test owns; cleaned in afterEach. */
function makeDir(): string {
  const d = mkdtempSync(join(tmpdir(), "mcp-ubt-guard-"));
  scratchDirs.push(d);
  return d;
}

beforeEach(() => {
  // Every test states its own env; start from "nothing configured".
  delete process.env.UNREAL_PROJECT_ROOT;
  delete process.env.UNREAL_ENGINE_ROOT;
});

afterEach(() => {
  // Restore the real env (other test files / the harness may rely on it) and
  // drop scratch dirs even when an assertion above failed.
  if (savedProjectRoot === undefined) delete process.env.UNREAL_PROJECT_ROOT;
  else process.env.UNREAL_PROJECT_ROOT = savedProjectRoot;
  if (savedEngineRoot === undefined) delete process.env.UNREAL_ENGINE_ROOT;
  else process.env.UNREAL_ENGINE_ROOT = savedEngineRoot;
  for (const d of scratchDirs.splice(0)) rmSync(d, { recursive: true, force: true });
});

describe("editor_build_game_target validation gates (no UBT spawn)", () => {
  covers("editor_build_game_target");

  test("no project_root arg and no UNREAL_PROJECT_ROOT: invalid_argument, actionable hint", async () => {
    const r: any = await buildGameTargetTool().handler({ project_root: "", target: "" }, ctx);
    expect(r.success).toBe(false);
    expect(r.error).toContain("No project root configured");
    expect(r.error_code).toBe("invalid_argument");
    expect(r.error_hint).toContain("UNREAL_PROJECT_ROOT");
  });

  // The next two tests encode host-Windows semantics: the handler discards a
  // Unix-shaped project_root ONLY when the build host is Windows (a container
  // caller passing "/workspace"). On a POSIX host "/..." is a native path and
  // is scanned directly, so these cases don't exist there.
  test.skipIf(process.platform !== "win32")("Unix-only path is rejected and falls back to the env root", async () => {
    // Env root is a REAL empty dir: if the fallback happens, the next gate
    // (the .uproject scan) fires and names THAT dir — proving "/workspace"
    // was discarded rather than scanned.
    const envRoot = makeDir();
    process.env.UNREAL_PROJECT_ROOT = envRoot;
    const r: any = await buildGameTargetTool().handler(
      { project_root: "/workspace", target: "" },
      ctx,
    );
    expect(r.success).toBe(false);
    expect(r.error_code).toBe("invalid_argument");
    expect(r.error).toContain(envRoot);
    expect(r.error).toContain("found 0");
  });

  test.skipIf(process.platform !== "win32")("Unix-only path with no env fallback: the no-root error, not a scan of '/workspace'", async () => {
    const r: any = await buildGameTargetTool().handler(
      { project_root: "/workspace", target: "" },
      ctx,
    );
    expect(r.success).toBe(false);
    expect(r.error).toContain("No project root configured");
    expect(r.error_code).toBe("invalid_argument");
  });

  test("project root with zero .uproject files: invalid_argument naming the count", async () => {
    const root = makeDir();
    const r: any = await buildGameTargetTool().handler({ project_root: root, target: "" }, ctx);
    expect(r.success).toBe(false);
    expect(r.error_code).toBe("invalid_argument");
    expect(r.error).toContain(root);
    expect(r.error).toContain("found 0");
    expect(r.error_hint).toContain(".uproject");
  });

  test("project root with two .uproject files: ambiguous, refused", async () => {
    const root = makeDir();
    writeFileSync(join(root, "GameA.uproject"), "{}");
    writeFileSync(join(root, "GameB.uproject"), "{}");
    const r: any = await buildGameTargetTool().handler({ project_root: root, target: "" }, ctx);
    expect(r.success).toBe(false);
    expect(r.error_code).toBe("invalid_argument");
    expect(r.error).toContain("found 2");
  });

  test("valid single-.uproject root but UNREAL_ENGINE_ROOT unset: engine-not-located, no spawn", async () => {
    const root = makeDir();
    writeFileSync(join(root, "Game.uproject"), "{}");
    const r: any = await buildGameTargetTool().handler({ project_root: root, target: "" }, ctx);
    expect(r.success).toBe(false);
    expect(r.error_code).toBe("invalid_argument");
    expect(r.error).toContain("Unreal engine not located");
    expect(r.error_hint).toContain("UNREAL_ENGINE_ROOT");
  });

  test("UNREAL_ENGINE_ROOT set but missing Engine/Build/BatchFiles/Build.bat: engine-not-located", async () => {
    const root = makeDir();
    writeFileSync(join(root, "Game.uproject"), "{}");
    // A real dir with the Engine/ skeleton but NO Build.bat — the stat probe
    // fails and the handler returns before spawnSync is ever reached.
    const engine = makeDir();
    mkdirSync(join(engine, "Engine", "Build", "BatchFiles"), { recursive: true });
    process.env.UNREAL_ENGINE_ROOT = engine;
    const r: any = await buildGameTargetTool().handler({ project_root: root, target: "" }, ctx);
    expect(r.success).toBe(false);
    expect(r.error_code).toBe("invalid_argument");
    expect(r.error).toContain("Unreal engine not located");
  });
});
