/**
 * EQS (Environment Query System) domain — author a UEnvQuery asset, add generator
 * options and scoring/filtering tests, mutate generator/test properties, read the
 * structure back, and remove tests/options. Port of tests/integration/test_eqs.py.
 *
 * Self-contained: EQS queries are authorable from scratch via the AIModule asset
 * factory, so nothing here is GUI-only. A module-scoped sample query is created
 * once and reused by the additive tests; the remove tests build their own
 * throwaway assets so option/test counts can be asserted exactly.
 */

import { test, expect, beforeAll } from "bun:test";
import { existsSync } from "node:fs";
import { join } from "node:path";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { ensureAbsent, assertReady } from "../harness/ops.ts";
import { projectDir } from "../harness/env.ts";

const NS = `${ROOT}/eqs`;
const SAMPLE = `${NS}/EQS_Sample`;

// Cache the discovered generator/test class names across the module so the
// introspection op is hit once rather than per test.
let discGenerator: string | undefined;
let discTest: string | undefined;

/** Map a /Game/... content path to its on-disk .uasset package file. */
function uassetDiskPath(gamePath: string): string {
  const pkg = gamePath.split(".")[0] ?? gamePath;
  const rel = pkg.slice("/Game/".length);
  return join(projectDir(), "Content", rel + ".uasset");
}

async function types(client: any, baseClass: string): Promise<any[]> {
  const result: any = await client.expect("eqs_list_types", { base_class: baseClass });
  return ((result.types as any[]) ?? []).filter((t) => t && typeof t === "object");
}

/** Discover a usable generator class (full class name). Prefer a simple,
 *  always-registered one; fall back to whatever the editor exposes first. */
async function pickGenerator(client: any): Promise<string> {
  if (discGenerator) return discGenerator;
  const names = (await types(client, "generator")).map((t) => t.class_name);
  let chosen: string | undefined = ["ActorsOfClass", "SimpleGrid", "OnCircle", "Donut"]
    .map((p) => `EnvQueryGenerator_${p}`)
    .find((n) => names.includes(n));
  chosen = chosen ?? (names.length ? names[0] : "EnvQueryGenerator_ActorsOfClass");
  discGenerator = chosen!;
  return discGenerator;
}

/** Discover a usable test class (full class name). */
async function pickTest(client: any): Promise<string> {
  if (discTest) return discTest;
  const names = (await types(client, "test")).map((t) => t.class_name);
  let chosen: string | undefined = ["Distance", "Dot", "Trace", "Random"]
    .map((p) => `EnvQueryTest_${p}`)
    .find((n) => names.includes(n));
  chosen = chosen ?? (names.length ? names[0] : "EnvQueryTest_Distance");
  discTest = chosen!;
  return discTest;
}

/** Return the option dict at `index` from a read_eqs_query result, or undefined. */
function option(readResult: any, index: any): any {
  for (const opt of (readResult.options as any[]) ?? []) {
    if (opt && typeof opt === "object" && opt.index === index) return opt;
  }
  return undefined;
}

/** Pick a trivially-settable scalar from a serialized property list
 *  ([{name,type,value}, ...]) returning [name, value, expected_token] or null. */
function settableScalar(props: any[]): [string, string | boolean | number, string] | null {
  const order: Record<string, number> = {
    FString: 0,
    FName: 0,
    bool: 1,
    int32: 2,
    int64: 2,
    int: 2,
    uint8: 2,
    uint32: 2,
    float: 3,
    double: 3,
  };
  let best: [string, string | boolean | number, string] | null = null;
  let bestRank = 99;
  for (const p of props) {
    if (!p || typeof p !== "object") continue;
    const name = p.name;
    const cpp = p.type;
    const rank = order[cpp];
    if (!name || rank === undefined || rank >= bestRank) continue;
    if (cpp === "FString" || cpp === "FName") best = [name, "MCPTestValue", "mcptestvalue"];
    else if (cpp === "bool") best = [name, true, "true"];
    else if (["int32", "int64", "int", "uint8", "uint32"].includes(cpp)) best = [name, 7, "7"];
    else best = [name, 1.5, "1.5"];
    bestRank = rank;
  }
  return best;
}

editorSuite("eqs", (ctx) => {
  // Module-scoped sample query for the additive tests.
  beforeAll(async () => {
    await ensureAbsent(ctx.mcp, SAMPLE);
    await ctx.mcp.expect("eqs_create", { asset_path: SAMPLE });
  });

  // ── introspection ──────────────────────────────────────────────────────────
  test("test_list_eqs_types", async () => {
    const result: any = await ctx.mcp.expect("eqs_list_types", { base_class: "all" });
    const eqsTypes = result.types as any[];
    expect(Array.isArray(eqsTypes)).toBe(true);
    expect(eqsTypes.length).toBeGreaterThan(0);
    expect(result.count).toEqual(eqsTypes.length);
    const cats = new Set(
      eqsTypes.filter((t) => t && typeof t === "object").map((t) => t.category),
    );
    expect(cats.has("generator") || cats.has("test")).toBe(true);
  });

  test("test_list_eqs_types_filtered_to_generators", async () => {
    const result: any = await ctx.mcp.expect("eqs_list_types", { base_class: "generator" });
    const eqsTypes = (result.types as any[]) ?? [];
    expect(eqsTypes.length).toBeGreaterThan(0);
    expect(eqsTypes.every((t) => t.category === "generator")).toBe(true);
  });

  // ── creation: the asset lands on disk ───────────────────────────────────────
  test("test_create_eqs_query_writes_uasset_on_disk", async () => {
    const path = `${NS}/EQS_Created`;
    await ensureAbsent(ctx.mcp, path);
    const result: any = await ctx.mcp.expect("eqs_create", { asset_path: path });
    expect(result.success).toBe(true);
    // create_eqs_query saves the package itself, so the .uasset must exist now.
    const disk = uassetDiskPath(path);
    expect(existsSync(disk)).toBe(true);
  });

  // ── options (generators) ────────────────────────────────────────────────────
  test("test_add_option_then_read", async () => {
    const gen = await pickGenerator(ctx.mcp);
    const added: any = await ctx.mcp.expect("eqs_option_add", {
      asset_path: SAMPLE,
      generator_type: gen,
    });
    const idx = added.option_index;
    expect(typeof idx === "number").toBe(true);
    expect(idx).toBeGreaterThanOrEqual(0);
    expect(added.generator_type).toEqual(gen);

    const read: any = await ctx.mcp.expect("eqs_read", { asset_path: SAMPLE });
    const opt = option(read, idx);
    expect(opt).toBeTruthy();
    expect((opt.generator ?? {}).type).toEqual(gen);
  });

  test("test_add_option_dry_run_does_not_mutate", async () => {
    const gen = await pickGenerator(ctx.mcp);
    const before: any = await ctx.mcp.expect("eqs_read", { asset_path: SAMPLE });
    const beforeCount = ((before.options as any[]) ?? []).length;

    const result: any = await ctx.mcp.expect("eqs_option_add", {
      asset_path: SAMPLE,
      generator_type: gen,
      dry_run: true,
    });
    expect(result.dry_run).toBe(true);
    const diff = (result.diff as any) ?? {};
    const added = (diff.options_added as any[]) ?? [];
    const entry = added.length ? added[0] : {};
    expect(entry.would_be_index).toEqual(beforeCount);
    expect(entry.generator_type).toEqual(gen);

    const after: any = await ctx.mcp.expect("eqs_read", { asset_path: SAMPLE });
    expect(((after.options as any[]) ?? []).length).toEqual(beforeCount);
  });

  // ── tests (scoring / filtering) ─────────────────────────────────────────────
  test("test_add_test_then_read", async () => {
    const gen = await pickGenerator(ctx.mcp);
    const testType = await pickTest(ctx.mcp);
    const optIdx = (
      await ctx.mcp.expect("eqs_option_add", { asset_path: SAMPLE, generator_type: gen })
    ).option_index;

    const added: any = await ctx.mcp.expect("eqs_test_add", {
      asset_path: SAMPLE,
      option_index: optIdx,
      test_type: testType,
    });
    const tidx = added.test_index;
    expect(typeof tidx === "number").toBe(true);
    expect(tidx).toBeGreaterThanOrEqual(0);
    expect(added.test_type).toEqual(testType);

    const read: any = await ctx.mcp.expect("eqs_read", { asset_path: SAMPLE });
    const opt = option(read, optIdx);
    expect(opt).toBeTruthy();
    const tests = (opt.tests as any[]) ?? [];
    const match = tests.find((t) => t.index === tidx);
    expect(match && match.type === testType).toBeTruthy();
  });

  // ── property mutation ───────────────────────────────────────────────────────
  test("test_set_generator_property", async () => {
    const gen = await pickGenerator(ctx.mcp);
    const optIdx = (
      await ctx.mcp.expect("eqs_option_add", { asset_path: SAMPLE, generator_type: gen })
    ).option_index;

    // Discover a settable scalar from the generator's live properties.
    const read: any = await ctx.mcp.expect("eqs_read", { asset_path: SAMPLE });
    const genProps = option(read, optIdx)?.generator?.properties ?? [];
    const chosen = settableScalar(genProps);
    expect(chosen).toBeTruthy();
    const [name, value, token] = chosen!;

    const result: any = await ctx.mcp.expect("eqs_set_property", {
      asset_path: SAMPLE,
      option_index: optIdx,
      target: "generator",
      property_name: name,
      property_value: value,
    });
    expect(result.success).toBe(true);

    // Read back: the named property's exported value must reflect the write.
    const read2: any = await ctx.mcp.expect("eqs_read", { asset_path: SAMPLE });
    const props2 = option(read2, optIdx)?.generator?.properties ?? [];
    const entry = props2.find((p: any) => p.name === name);
    expect(entry).toBeTruthy();
    expect(String(entry.value).toLowerCase()).toContain(token);
  });

  test("test_set_test_property", async () => {
    const gen = await pickGenerator(ctx.mcp);
    const testType = await pickTest(ctx.mcp);
    const optIdx = (
      await ctx.mcp.expect("eqs_option_add", { asset_path: SAMPLE, generator_type: gen })
    ).option_index;
    const tidx = (
      await ctx.mcp.expect("eqs_test_add", {
        asset_path: SAMPLE,
        option_index: optIdx,
        test_type: testType,
      })
    ).test_index;

    const read: any = await ctx.mcp.expect("eqs_read", { asset_path: SAMPLE });
    const testEntry = (option(read, optIdx)?.tests ?? []).find((t: any) => t.index === tidx);
    expect(testEntry).toBeTruthy();
    const chosen = settableScalar(testEntry.properties ?? []);
    if (!chosen) {
      console.log(`test ${testType} exposed no trivially-settable scalar property`);
      return;
    }
    const [name, value, token] = chosen;

    // target is the numeric test index passed as a string (handler Atoi-parses it).
    const result: any = await ctx.mcp.expect("eqs_set_property", {
      asset_path: SAMPLE,
      option_index: optIdx,
      target: String(tidx),
      property_name: name,
      property_value: value,
    });
    expect(result.success).toBe(true);

    const read2: any = await ctx.mcp.expect("eqs_read", { asset_path: SAMPLE });
    const test2 = (option(read2, optIdx)?.tests ?? []).find((t: any) => t.index === tidx);
    const entry = ((test2?.properties ?? []) as any[]).find((p: any) => p.name === name);
    expect(entry).toBeTruthy();
    expect(String(entry.value).toLowerCase()).toContain(token);
  });

  // ── removal (own assets so counts assert exactly) ───────────────────────────
  test("test_remove_test", async () => {
    const path = `${NS}/EQS_RemoveTest`;
    await ensureAbsent(ctx.mcp, path);
    await ctx.mcp.expect("eqs_create", { asset_path: path });
    const gen = await pickGenerator(ctx.mcp);
    const testType = await pickTest(ctx.mcp);
    await ctx.mcp.expect("eqs_option_add", { asset_path: path, generator_type: gen });
    // Two tests on option 0; remove the first, the second must survive.
    await ctx.mcp.expect("eqs_test_add", { asset_path: path, option_index: 0, test_type: testType });
    await ctx.mcp.expect("eqs_test_add", { asset_path: path, option_index: 0, test_type: testType });

    const removed: any = await ctx.mcp.expect("eqs_test_remove", {
      asset_path: path,
      option_index: 0,
      test_index: 0,
    });
    expect(removed.success).toBe(true);
    await assertReady(ctx.mcp);

    const read: any = await ctx.mcp.expect("eqs_read", { asset_path: path });
    const opt = option(read, 0);
    expect(opt).toBeTruthy();
    expect(((opt.tests as any[]) ?? []).length).toEqual(1);
  });

  test("test_remove_option", async () => {
    const path = `${NS}/EQS_RemoveOption`;
    await ensureAbsent(ctx.mcp, path);
    await ctx.mcp.expect("eqs_create", { asset_path: path });
    const gen = await pickGenerator(ctx.mcp);
    await ctx.mcp.expect("eqs_option_add", { asset_path: path, generator_type: gen });
    expect(
      (((await ctx.mcp.expect("eqs_read", { asset_path: path })).options as any[]) ?? []).length,
    ).toEqual(1);

    const removed: any = await ctx.mcp.expect("eqs_option_remove", {
      asset_path: path,
      option_index: 0,
    });
    expect(removed.success).toBe(true);
    await assertReady(ctx.mcp);

    const read: any = await ctx.mcp.expect("eqs_read", { asset_path: path });
    expect(((read.options as any[]) ?? []).length).toEqual(0);
  });

  test("test_remove_option_dry_run_does_not_mutate", async () => {
    const path = `${NS}/EQS_RemoveOptionDry`;
    await ensureAbsent(ctx.mcp, path);
    await ctx.mcp.expect("eqs_create", { asset_path: path });
    const gen = await pickGenerator(ctx.mcp);
    await ctx.mcp.expect("eqs_option_add", { asset_path: path, generator_type: gen });

    const result: any = await ctx.mcp.expect("eqs_option_remove", {
      asset_path: path,
      option_index: 0,
      dry_run: true,
    });
    expect(result.dry_run).toBe(true);
    const removedList = ((result.diff as any)?.options_removed as any[]) ?? [];
    const entry = removedList.length ? removedList[0] : {};
    expect(entry.option_index).toEqual(0);
    expect(entry.generator_type).toEqual(gen);

    // The option must still be present after a dry-run remove.
    const read: any = await ctx.mcp.expect("eqs_read", { asset_path: path });
    expect(((read.options as any[]) ?? []).length).toEqual(1);
  });
});
