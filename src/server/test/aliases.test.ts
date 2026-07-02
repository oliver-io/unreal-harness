import { expect, test, describe } from "bun:test";
import { z } from "zod";
import {
  applyAliases,
  withAssetAliases,
  withBpAliases,
  withMaterialAliases,
} from "../src/registry/aliases.ts";
import { ToolRegistry } from "../src/registry/index.ts";
import { metaTools } from "../src/disclosure/metatools.ts";
import { defineTool } from "../src/domains/_shared.ts";
import type { ToolContext, ToolDef } from "../src/registry/types.ts";

const ctx = { conn: undefined } as unknown as ToolContext;

// Stand-in for a forwarding tool: its handler echoes the (already-normalized)
// params, so a test can assert exactly what the canonical handler received.
function echoTool(name: string, domain: string, input: z.ZodObject<z.ZodRawShape>): ToolDef {
  return defineTool({
    name,
    domain,
    description: `echo ${name}`,
    input,
    handler: (a) => ({ status: "success", echoed: a }),
  });
}

describe("applyAliases (normalization)", () => {
  const aliases = { blueprint_name: "blueprint_path", bp_path: "blueprint_path" };

  test("folds an alias key onto the canonical key and drops the alias", () => {
    const out = applyAliases({ blueprint_name: "/Game/BP_X" }, aliases);
    expect(out.blueprint_path).toBe("/Game/BP_X");
    expect("blueprint_name" in out).toBe(false);
  });

  test("canonical wins when both canonical and alias are present", () => {
    const out = applyAliases(
      { blueprint_path: "/Game/Canon", blueprint_name: "/Game/Alias" },
      aliases,
    );
    expect(out.blueprint_path).toBe("/Game/Canon");
    expect("blueprint_name" in out).toBe(false);
  });

  test("no aliases → returns the same reference untouched", () => {
    const args = { x: 1 };
    expect(applyAliases(args, undefined)).toBe(args);
  });
});

describe("withBpAliases (schema widening)", () => {
  test("a blueprint_path tool also accepts blueprint_name / bp_path", () => {
    const [def] = withBpAliases([
      echoTool("bp_read", "bp", z.object({ blueprint_path: z.string().min(1) })),
    ]);
    expect(def!.aliases).toEqual({
      blueprint_name: "blueprint_path",
      bp_path: "blueprint_path",
    });
    // alias keys are now part of the schema (so the SDK won't strip them)…
    expect("blueprint_name" in def!.input.shape).toBe(true);
    // …and the canonical is relaxed to optional (satisfiable via an alias).
    expect(def!.input.safeParse({ blueprint_name: "/Game/BP" }).success).toBe(true);
  });

  test("the alias direction inverts for a blueprint_name tool", () => {
    const [def] = withBpAliases([
      echoTool("bp_compile", "bp", z.object({ blueprint_name: z.string().min(1) })),
    ]);
    expect(def!.aliases).toEqual({
      blueprint_path: "blueprint_name",
      bp_path: "blueprint_name",
    });
  });
});

describe("material / asset alias widening", () => {
  test("material_path tool gains path + asset_path aliases", () => {
    const [def] = withMaterialAliases([
      echoTool("material_read", "material", z.object({ material_path: z.string().min(1) })),
    ]);
    expect(def!.aliases).toEqual({ path: "material_path", asset_path: "material_path" });
  });

  test("asset_path tool gains a generic path alias", () => {
    const [def] = withAssetAliases([
      echoTool("asset_delete", "asset", z.object({ asset_path: z.string() })),
    ]);
    expect(def!.aliases).toEqual({ path: "asset_path" });
  });
});

describe("end-to-end via catalog_call (validate → normalize → handler)", () => {
  function setup() {
    const registry = new ToolRegistry();
    registry.registerAll(
      withBpAliases([echoTool("bp_read", "bp", z.object({ blueprint_path: z.string().min(1) }))]),
    );
    registry.registerAll(
      withAssetAliases([echoTool("asset_delete", "asset", z.object({ asset_path: z.string() }))]),
    );
    const meta = metaTools(registry);
    registry.registerAll(meta);
    return meta.find((t) => t.name === "catalog_call")!;
  }

  test("bp_read accepts blueprint_name and the handler receives blueprint_path", async () => {
    const call = setup();
    const r: any = await call.handler(
      { name: "bp_read", params: { blueprint_name: "/Game/BP_Hero" } },
      ctx,
    );
    expect(r.echoed.blueprint_path).toBe("/Game/BP_Hero");
    expect("blueprint_name" in r.echoed).toBe(false);
  });

  test("asset_delete accepts a generic path alias → asset_path", async () => {
    const call = setup();
    const r: any = await call.handler(
      { name: "asset_delete", params: { path: "/Game/Junk/BP_Old" } },
      ctx,
    );
    expect(r.echoed.asset_path).toBe("/Game/Junk/BP_Old");
    expect("path" in r.echoed).toBe(false);
  });
});
