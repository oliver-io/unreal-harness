/**
 * Editor-gated suite helper for ported integration tests. Mirrors the pytest
 * conftest fixtures: each suite gets a live in-process MCP client (`mcp`, tool
 * names) and a raw bridge client (`bridge`, wire names), and is SKIPPED when no
 * interactive editor is on :55557 — so `bun test` is green on a dev box with no
 * engine and exercises real round-trips in CI / against a running editor.
 */

import { describe, beforeAll, afterAll } from "bun:test";
import { resolve } from "node:path";
import { startTestClient, type TestClient } from "./mcpClient.ts";
import { RawBridge } from "./bridgeClient.ts";
import { editorReady } from "./bridge.ts";
import { projectDir, BRIDGE_PORT } from "./env.ts";

/** True iff the interactive editor on the bridge port is hosting the TEST
 *  project (`UE_MCP_TEST_PROJECT`, default the fixture project). An editor
 *  hosting any OTHER project — typically someone's real game under projects/ —
 *  is treated as NOT live and every suite skips: attaching blindly is how test
 *  mutations end up inside a real project's Content. Identity comes from the
 *  editor's own `project_context` (settings_paths[0] == FPaths::ProjectDir()).
 *  Escape hatch: UE_MCP_ATTACH_ANY=1 attaches to whatever is there. */
async function liveTestEditor(): Promise<boolean> {
  if (!(await editorReady())) return false;
  if (process.env.UE_MCP_ATTACH_ANY === "1") return true;
  try {
    const ctx = await new RawBridge().expect("project_context", {});
    const hosted = resolve(String((ctx.settings_paths as string[] | undefined)?.[0] ?? ""));
    const expected = resolve(projectDir());
    const norm = (p: string) => (process.platform === "win32" ? p.toLowerCase() : p);
    if (norm(hosted) === norm(expected)) return true;
    console.warn(
      `editor on :${BRIDGE_PORT} hosts '${hosted}', not the test project '${expected}' — ` +
        "editor suites SKIPPED so tests never mutate a project they don't own. " +
        "Point UE_MCP_TEST_PROJECT at that project to target it deliberately, " +
        "or set UE_MCP_ATTACH_ANY=1 to bypass this guard.",
    );
    return false;
  } catch {
    return false;
  }
}

/** Probed once at import — true iff an interactive editor hosting the test
 *  project is on the bridge port. */
export const LIVE: boolean = await liveTestEditor();

/** Render/screenshot/window tests need a real RHI (a GUI editor). Off under the
 *  default headless -nullrhi editor; set UE_MCP_GUI=1 when running a GUI editor.
 *  Use `test.skipIf(!GUI)(...)` for those cases. */
export const GUI: boolean = process.env.UE_MCP_GUI === "1";

/** Shared on-disk scratch namespace (matches the Python harness). */
export const NS = "/Game/__MCPTest__";

export interface Ctx {
  mcp: TestClient;
  bridge: RawBridge;
}

/**
 * Declare an editor-dependent suite. The body runs synchronously to register
 * tests; `ctx.mcp` is connected in beforeAll, so reference `ctx.mcp` / `ctx.bridge`
 * inside test() / beforeAll callbacks (not at registration time).
 */
export function editorSuite(name: string, body: (ctx: Ctx) => void): void {
  describe.skipIf(!LIVE)(name, () => {
    const ctx: Ctx = { mcp: undefined as unknown as TestClient, bridge: new RawBridge() };
    beforeAll(async () => {
      ctx.mcp = await startTestClient("full");
    });
    afterAll(async () => {
      await ctx.mcp?.close();
    });
    body(ctx);
  });
}
