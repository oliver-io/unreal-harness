/**
 * Editor-gated suite helper for ported integration tests. Mirrors the pytest
 * conftest fixtures: each suite gets a live in-process MCP client (`mcp`, tool
 * names) and a raw bridge client (`bridge`, wire names), and is SKIPPED when no
 * interactive editor is on :55557 — so `bun test` is green on a dev box with no
 * engine and exercises real round-trips in CI / against a running editor.
 */

import { describe, beforeAll, afterAll } from "bun:test";
import { startTestClient, type TestClient } from "./mcpClient.ts";
import { RawBridge } from "./bridgeClient.ts";
import { editorReady } from "./bridge.ts";

/** Probed once at import — true iff an interactive editor is on :55557. */
export const LIVE: boolean = await editorReady();

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
