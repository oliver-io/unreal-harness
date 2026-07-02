import { expect, test, describe } from "bun:test";
import { z } from "zod";
import { ToolRegistry } from "../src/registry/index.ts";
import { defineTool } from "../src/domains/_shared.ts";
import { metaTools } from "../src/disclosure/metatools.ts";
import { codeTools } from "../src/disclosure/codemode/tools.ts";
import type { ToolContext, ToolDef } from "../src/registry/types.ts";

const ctx = { conn: undefined } as unknown as ToolContext;

function setup() {
  const registry = new ToolRegistry();
  registry.register(
    defineTool({
      name: "actor_get_in_level",
      domain: "actor",
      description: "Echo tool for tests.",
      input: z.object({ x: z.number().default(1) }),
      annotations: { readOnlyHint: true },
      handler: (a) => ({ status: "success", result: { echo: a.x } }),
    }),
  );
  const meta = metaTools(registry);
  const code = codeTools(registry);
  registry.registerAll(meta);
  registry.registerAll(code);
  const get = (name: string): ToolDef => {
    const d = [...meta, ...code].find((t) => t.name === name);
    if (!d) throw new Error(name);
    return d;
  };
  return { registry, get };
}

describe("tier 2 — catalog meta-tools", () => {
  test("catalog_domains hides catalog/code plumbing", async () => {
    const { get } = setup();
    const r: any = await get("catalog_domains").handler({}, ctx);
    expect(r.result.domains.map((d: any) => d.domain)).toEqual(["actor"]);
    expect(r.result.total).toBe(1);
  });

  test("catalog_search finds domain tools, excludes meta", async () => {
    const { get } = setup();
    const r: any = await get("catalog_search").handler({ query: "echo", limit: 20 }, ctx);
    expect(r.result.matches.some((m: any) => m.name === "actor_get_in_level")).toBe(true);
    expect(r.result.matches.some((m: any) => m.domain === "catalog")).toBe(false);
  });

  test("catalog_describe returns schema + pie/dryrun annotations", async () => {
    const { get } = setup();
    const r: any = await get("catalog_describe").handler({ name: "actor_get_in_level" }, ctx);
    expect(r.result.inputSchema.type).toBe("object");
    expect(r.result.annotations).toHaveProperty("blockedDuringPie");
    const miss: any = await get("catalog_describe").handler({ name: "nope" }, ctx);
    expect(miss.status).toBe("error");
  });

  test("catalog_call dispatches + validates", async () => {
    const { get } = setup();
    const ok: any = await get("catalog_call").handler({ name: "actor_get_in_level", params: { x: 5 } }, ctx);
    expect(ok.result.echo).toBe(5);
    const bad: any = await get("catalog_call").handler({ name: "actor_get_in_level", params: { x: "no" } }, ctx);
    expect(bad.status).toBe("error");
    expect(bad.error_code).toBe("invalid_argument");
  });
});

describe("tier 3 — code-execution mode", () => {
  test("code_api lists callable unreal.* functions", async () => {
    const { get } = setup();
    const r: any = await get("code_api").handler({}, ctx);
    expect(r.result.api).toContain("unreal.actor_get_in_level".replace("unreal.", ""));
    expect(r.result.api).toContain("actor_get_in_level(params");
  });

  test("code_run executes a snippet, returns logs + value, keeps work in sandbox", async () => {
    const { get } = setup();
    const code = `
      const results = [];
      for (let i = 0; i < 3; i++) results.push((await unreal.actor_get_in_level({ x: i })).result.echo);
      console.log("sum", results.reduce((a, b) => a + b, 0));
      return results.length;
    `;
    const r: any = await get("code_run").handler({ code, timeout_ms: 10000 }, ctx);
    expect(r.status).toBe("success");
    expect(r.result.value).toBe(3);
    expect(r.result.calls).toBe(3);
    expect(r.result.logs.join(" ")).toContain("sum 3");
  });

  test("code_run surfaces errors from the snippet", async () => {
    const { get } = setup();
    const r: any = await get("code_run").handler({ code: `throw new Error("boom")`, timeout_ms: 5000 }, ctx);
    expect(r.status).toBe("error");
    expect(r.error).toContain("boom");
  });
});
