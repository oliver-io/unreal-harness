/**
 * Protocol-path integration: a real MCP client ↔ the real server over in-memory
 * transport. Exercises tool advertising, surface-mode filtering, schema
 * validation, and the catalog/code disclosure tools — none of which need a live
 * editor. (Editor-dependent ops live in editor.test.ts, gated on :55557.)
 */

import { expect, test, describe } from "bun:test";
import { startTestClient } from "../harness/mcpClient.ts";

describe("MCP protocol surface", () => {
  test("full mode advertises the domain tools + catalog_*, hides code_*", async () => {
    const c = await startTestClient("full");
    try {
      const names = await c.listToolNames();
      expect(names).toContain("actor_spawn");
      expect(names).toContain("catalog_search");
      expect(names).toContain("mcp_status");
      expect(names).not.toContain("code_run");
      expect(names.length).toBeGreaterThan(200);
    } finally {
      await c.close();
    }
  });

  test("compact mode advertises only the plumbing tools", async () => {
    const c = await startTestClient("compact");
    try {
      const names = await c.listToolNames();
      expect(names).toContain("catalog_search");
      expect(names).toContain("result_read");
      expect(names).not.toContain("actor_spawn");
      expect(names.length).toBeLessThan(12);
    } finally {
      await c.close();
    }
  });

  test("code mode adds code_api + code_run", async () => {
    const c = await startTestClient("code");
    try {
      const names = await c.listToolNames();
      expect(names).toContain("code_run");
      expect(names).toContain("code_api");
      expect(names).not.toContain("actor_spawn");
    } finally {
      await c.close();
    }
  });

  test("catalog_search → catalog_describe disclosure flow works over MCP", async () => {
    const c = await startTestClient("compact");
    try {
      const search = await c.call("catalog_search", { query: "spawn actor" });
      const matches = (search.result as { matches: { name: string }[] }).matches;
      expect(matches.some((m) => m.name === "actor_spawn")).toBe(true);

      const desc = await c.call("catalog_describe", { name: "actor_spawn" });
      const schema = (desc.result as { inputSchema: { properties: Record<string, unknown> } }).inputSchema;
      expect(schema.properties).toHaveProperty("class_path");
    } finally {
      await c.close();
    }
  });

  test("schema validation rejects bad args at the protocol layer", async () => {
    const c = await startTestClient("full");
    try {
      // actor_spawn requires class_path:string; omitting it must error.
      await expect(c.call("actor_spawn", {})).rejects.toBeDefined();
    } finally {
      await c.close();
    }
  });

  test("code_api lists callable unreal.* functions", async () => {
    const c = await startTestClient("code");
    try {
      const api = await c.call("code_api", { domain: "actor" });
      expect((api.result as { api: string }).api).toContain("actor_spawn(params");
    } finally {
      await c.close();
    }
  });
});
