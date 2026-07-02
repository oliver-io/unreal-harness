/**
 * Editor-dependent integration: real MCP client → server → bridge → LIVE editor.
 *
 * Gated on a reachable, interactive editor at :55557. If none is up (and
 * UE_MCP_LIVE!=1), the whole suite SKIPS rather than fails — so `bun test` is
 * green on a dev box with no engine, and exercises real round-trips in CI where
 * an editor is launched. Set UE_MCP_LIVE=1 to have the suite launch its own
 * headless editor (slow first build).
 */

import { expect, test, describe, beforeAll, afterAll } from "bun:test";
import { startTestClient, type TestClient } from "../harness/mcpClient.ts";
import { editorReady } from "../harness/bridge.ts";
import { EditorSession } from "../harness/editor.ts";

let session: EditorSession | undefined;
let live = await editorReady();

if (!live && process.env.UE_MCP_LIVE === "1") {
  session = await new EditorSession().start("auto");
  live = await editorReady();
}

describe.skipIf(!live)("live editor round-trips", () => {
  let c: TestClient;
  beforeAll(async () => {
    c = await startTestClient("full");
  });
  afterAll(async () => {
    await c?.close();
    await session?.stop();
  });

  test("mcp_status reports ready", async () => {
    const env = await c.call("mcp_status");
    expect(env.status).toBe("success");
    expect((env.result as { ready: boolean }).ready).toBe(true);
  });

  test("actor_get_in_level returns the level actor list", async () => {
    const env = await c.call("actor_get_in_level");
    expect(env.status).toBe("success");
  });

  test("actor_spawn dry_run validates without mutating", async () => {
    const env = await c.call("actor_spawn", {
      class_path: "/Script/Engine.PointLight",
      dry_run: true,
    });
    expect(env.status).toBe("success");
    expect((env.result as { dry_run?: boolean }).dry_run).toBe(true);
  });

  test("catalog_call reaches a domain tool by name", async () => {
    const env = await c.call("catalog_call", { name: "actor_get_in_level", params: {} });
    expect(env.status).toBe("success");
  });

  test("code_run drives the editor and keeps data in-sandbox", async () => {
    const code = `
      const env = await unreal.actor_get_in_level({});
      const actors = env.result?.actors ?? [];
      console.log("actor count", actors.length);
      return { ok: env.status === "success", count: actors.length };
    `;
    const cc = await startTestClient("code");
    try {
      const env = await cc.call("code_run", { code, timeout_ms: 20000 });
      expect(env.status).toBe("success");
      expect((env.result as { value: { ok: boolean } }).value.ok).toBe(true);
    } finally {
      await cc.close();
    }
  });
});
