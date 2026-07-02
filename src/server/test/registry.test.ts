import { expect, test, describe } from "bun:test";
import { z } from "zod";
import { ToolRegistry } from "../src/registry/index.ts";
import { bridgeTool } from "../src/domains/_shared.ts";

function fixtureRegistry(): ToolRegistry {
  const r = new ToolRegistry();
  r.registerAll([
    bridgeTool({
      name: "actor_spawn",
      domain: "actor",
      description: "Spawn one actor at a transform.",
      input: z.object({ class_path: z.string() }),
    }),
    bridgeTool({
      name: "actor_delete",
      domain: "actor",
      description: "Delete an actor by name.",
      input: z.object({ name: z.string() }),
    }),
    bridgeTool({
      name: "asset_list",
      domain: "asset",
      description: "List assets under a directory.",
      input: z.object({ directory_path: z.string().default("/Game/") }),
    }),
  ]);
  return r;
}

describe("registry", () => {
  test("register + size + duplicate guard", () => {
    const r = fixtureRegistry();
    expect(r.size()).toBe(3);
    expect(r.has("actor_spawn")).toBe(true);
    expect(() =>
      r.register(bridgeTool({
        name: "actor_spawn",
        domain: "actor",
        description: "dup",
        input: z.object({}),
      })),
    ).toThrow(/duplicate/);
  });

  test("domains aggregates counts", () => {
    expect(fixtureRegistry().domains()).toEqual([
      { domain: "actor", count: 2 },
      { domain: "asset", count: 1 },
    ]);
  });

  test("search ranks exact name highest, supports domain filter", () => {
    const r = fixtureRegistry();
    const hits = r.search("actor_spawn");
    expect(hits[0]?.name).toBe("actor_spawn");

    const assetOnly = r.search("list", { domain: "asset" });
    expect(assetOnly.every((h) => h.domain === "asset")).toBe(true);
    expect(assetOnly[0]?.name).toBe("asset_list");
  });

  test("empty query lists everything within limit", () => {
    expect(fixtureRegistry().search("").length).toBe(3);
  });

  test("inputJsonSchema produces a JSON Schema for a tool", () => {
    const schema = fixtureRegistry().inputJsonSchema("actor_spawn");
    expect(schema).toBeDefined();
    expect((schema as { type?: string }).type).toBe("object");
  });
});
