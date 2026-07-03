/**
 * StateTree domain — author a StateTree asset, mutate its state hierarchy,
 * nodes, transitions and property bindings, compile/verify it, and read the
 * resulting state back. Port of tests/integration/test_statetree.py.
 *
 * Self-contained: StateTree is authorable from scratch. A module-scoped sample
 * StateTree is created once under the test namespace (in beforeAll) and reused
 * by the mutation tests; each adds its own uniquely-named states and references
 * them by GUID, so the tests stay independent of one another's ordering.
 *
 * Every call uses ctx.mcp (canonical tool names) — the Python helpers were named
 * `bridge` but were always passed the MCP client.
 */

import { test, expect, beforeAll } from "bun:test";
import { existsSync, statSync } from "node:fs";
import { join } from "node:path";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { ensureAbsent, assertReady } from "../harness/ops.ts";
import { projectDir } from "../harness/env.ts";

const NS = `${ROOT}/statetree`;
const SAMPLE = `${NS}/ST_Sample`;

/** Map a /Game/... content path to its on-disk package file (mirrors
 *  config.uasset_disk_path). The file only exists after the asset is SAVED. */
function uassetDiskPath(gamePath: string, ext = ".uasset"): string {
  const pkg = gamePath.split(".")[0] ?? gamePath;
  if (!pkg.startsWith("/Game/")) throw new Error(`not a /Game/ path: ${gamePath}`);
  const rel = pkg.slice("/Game/".length);
  return join(projectDir(), "Content", rel + ext);
}

function isFile(p: string): boolean {
  return existsSync(p) && statSync(p).isFile();
}

// Cache the discovered schema / task-node-type across the module so we hit the
// introspection ops once rather than per test.
let discoverySchema: string | undefined;
let taskComputed = false;
let discoveryTask: string | null = null;

/** Discover a usable StateTree schema class. statetree_create requires one
 *  (the factory returns nullptr if the schema is unset before asset creation).
 *  Prefer the plain component schema; fall back to whatever the editor exposes. */
async function pickSchema(client: any): Promise<string> {
  if (discoverySchema) return discoverySchema;
  const result = await client.expect("statetree_list_schemas", {});
  const names: string[] = ((result.schemas as any[]) || [])
    .filter((s) => s && typeof s === "object")
    .map((s) => s.class_name);
  const schema: string =
    ["StateTreeComponentSchema", "StateTreeAIComponentSchema"].find((p) => names.includes(p)) ||
    (names.length ? names[0]! : "StateTreeComponentSchema");
  discoverySchema = schema;
  return schema;
}

/** Return the canonical short name of an available task node type (e.g.
 *  "STTask_...") or null if the editor exposes no StateTree task structs. */
async function firstTaskType(client: any): Promise<string | null> {
  if (taskComputed) return discoveryTask;
  const result = await client.expect("statetree_list_node_types", { base_class: "task" });
  const types = (result.types as any[]) || [];
  const name = types.length && types[0] && typeof types[0] === "object" ? types[0].name : null;
  discoveryTask = name ?? null;
  taskComputed = true;
  return discoveryTask;
}

/** Add a state and return its GUID. States are always referenced by GUID
 *  afterwards so duplicate display-names across tests never cause ambiguity. */
async function addState(client: any, asset: string, name: string, parent?: string): Promise<string> {
  const params: any = { asset_path: asset, name, state_type: "State", enabled: true };
  if (parent) params.parent = parent;
  const result = await client.expect("statetree_state_add", params);
  const sid = result.state_id;
  expect(sid).toBeTruthy();
  return sid;
}

/** Attach a task node to a state and return its node GUID. */
async function addTask(client: any, asset: string, stateId: string, nodeType: string): Promise<string> {
  const result = await client.expect("statetree_node_add", {
    asset_path: asset,
    node_type: nodeType,
    target: { slot: "task", state: stateId },
  });
  const nid = result.node_id;
  expect(nid).toBeTruthy();
  return nid;
}

/** Pick a trivially-settable (scalar) property from a node's serialized
 *  instance_data, returning [name, value, matcher] — matcher validates the
 *  re-read serialized text form ("7.250000", "True", "42", "MCPTest").
 *  Values are DISTINCTIVE so a readback can never pass on defaults. Avoids
 *  struct/enum props whose text import is finicky. */
function settableProperty(props: any[]): [string, any, (v: unknown) => boolean] | null {
  for (const p of props) {
    if (!p || typeof p !== "object") continue;
    const name = p.name,
      cpp = p.type;
    if (!name) continue;
    if (cpp === "bool") return [name, true, (v) => String(v) === "True"];
    if (cpp === "float" || cpp === "double")
      return [name, 7.25, (v) => Math.abs(Number(v) - 7.25) < 1e-4];
    if (["int32", "int64", "int", "uint8", "uint32"].includes(cpp))
      return [name, 42, (v) => Math.trunc(Number(v)) === 42];
    if (cpp === "FString" || cpp === "FName") return [name, "MCPTest", (v) => String(v) === "MCPTest"];
  }
  return null;
}

// Cache for the scalar-bearing task type (see scalarTaskType).
let scalarTaskComputed = false;
let scalarTask: string | null = null;

/** A task node type whose instance data carries a plain scalar, so the
 *  set-property test can actually WRITE and RE-READ a value. Prefers the
 *  engine-shipped StateTreeDelayTask (Duration float); falls back to the first
 *  type advertising a scalar (the generic types[0] pick is often
 *  StateTreeBlueprintTaskWrapper, which has no instance properties). */
async function scalarTaskType(client: any): Promise<string | null> {
  if (scalarTaskComputed) return scalarTask;
  const result = await client.expect("statetree_list_node_types", { base_class: "task" });
  const types = ((result.types as any[]) || []).filter((t) => t && typeof t === "object");
  const scalarKinds = new Set([
    "bool", "float", "double", "int32", "int64", "int", "uint8", "uint32", "FString", "FName",
  ]);
  const hasScalar = (t: any): boolean =>
    ((t.properties as any[]) || []).some((p) => p && typeof p === "object" && scalarKinds.has(p.type));
  if (types.some((t) => t.name === "StateTreeDelayTask" && hasScalar(t))) {
    scalarTask = "StateTreeDelayTask";
  } else {
    scalarTask = types.find(hasScalar)?.name ?? null;
  }
  scalarTaskComputed = true;
  return scalarTask;
}

editorSuite("statetree", (ctx) => {
  let sampleSt: string;

  beforeAll(async () => {
    // Create one StateTree asset for the whole module.
    await ensureAbsent(ctx.mcp, SAMPLE);
    await ctx.mcp.expect("statetree_create", {
      asset_path: SAMPLE,
      schema_class: await pickSchema(ctx.mcp),
    });
    sampleSt = SAMPLE;
  });

  // ── introspection ──────────────────────────────────────────────────────────
  test("test_list_state_tree_schemas", async () => {
    const result: any = await ctx.mcp.expect("statetree_list_schemas", {});
    const schemas = result.schemas;
    expect(Array.isArray(schemas) && schemas.length).toBeTruthy();
    for (const s of schemas) expect(s.class_name).toBeDefined();
  });

  test("test_list_state_tree_node_types", async () => {
    const result: any = await ctx.mcp.expect("statetree_list_node_types", { base_class: "all" });
    const types = result.types;
    expect(Array.isArray(types)).toBe(true);
    expect(result.count).toEqual(types.length);
  });

  // ── creation: persisted assets land on disk ────────────────────────────────
  test("test_create_state_tree_writes_uasset_on_disk", async () => {
    const path = `${NS}/ST_Created`;
    await ensureAbsent(ctx.mcp, path);
    const result: any = await ctx.mcp.expect("statetree_create", {
      asset_path: path,
      schema_class: await pickSchema(ctx.mcp),
    });
    expect(result.success).toBe(true);
    // statetree_create saves the package itself, so the .uasset must exist now.
    const disk = uassetDiskPath(path);
    expect(isFile(disk)).toBe(true);
    // Raw save so the disk-write is re-confirmed; save must report success.
    const saved: any = await ctx.mcp.expect("statetree_save", { asset_path: path });
    expect(saved.success).toBe(true);
  });

  // ── state hierarchy ────────────────────────────────────────────────────────
  test("test_add_state_then_list", async () => {
    const sid = await addState(ctx.mcp, sampleSt, "S_AddList");
    const result: any = await ctx.mcp.expect("statetree_state_list", { asset_path: sampleSt });
    const states = (result.states as any[]) || [];
    const ids = new Set(states.map((s) => s.id));
    expect(ids.has(sid)).toBe(true);
  });

  test("test_read_state_tree", async () => {
    const sid = await addState(ctx.mcp, sampleSt, "S_Read");
    const result: any = await ctx.mcp.expect("statetree_read", {
      asset_path: sampleSt,
      include_node_properties: true,
      include_bindings: true,
    });
    expect(JSON.stringify(result.states)).toContain(sid);
    expect(result.asset_path).toBeTruthy();
  });

  test("test_rename_state", async () => {
    const sid = await addState(ctx.mcp, sampleSt, "S_RenameBefore");
    const result: any = await ctx.mcp.expect("statetree_state_rename", {
      asset_path: sampleSt,
      state: sid,
      new_name: "S_RenameAfter",
    });
    expect(result.new_name).toEqual("S_RenameAfter");
    const states =
      ((await ctx.mcp.expect("statetree_state_list", { asset_path: sampleSt })).states as any[]) ||
      [];
    const entry = states.find((s) => s.id === sid);
    expect(entry && entry.name === "S_RenameAfter").toBeTruthy();
  });

  test("test_set_state_properties", async () => {
    const sid = await addState(ctx.mcp, sampleSt, "S_SetProps");
    await ctx.mcp.expect("statetree_state_set_properties", {
      asset_path: sampleSt,
      state: sid,
      selection_behavior: "TrySelectChildrenInOrder",
      enabled: false,
      weight: 2.0,
    });
    const states =
      ((await ctx.mcp.expect("statetree_state_list", { asset_path: sampleSt })).states as any[]) ||
      [];
    const entry: any = states.find((s) => s.id === sid);
    expect(entry).toBeTruthy();
    expect(entry.selection_behavior).toEqual("TrySelectChildrenInOrder");
    expect(entry.enabled).toBe(false);
    expect(entry.weight).toBeCloseTo(2.0);
  });

  test("test_move_state", async () => {
    const parent = await addState(ctx.mcp, sampleSt, "S_MoveParent");
    const child = await addState(ctx.mcp, sampleSt, "S_MoveChild", parent);
    // Reparent the child up to the root level (omit new_parent -> root).
    const result: any = await ctx.mcp.expect("statetree_state_move", {
      asset_path: sampleSt,
      state: child,
    });
    expect(result.state_id).toEqual(child);
    const states =
      ((await ctx.mcp.expect("statetree_state_list", { asset_path: sampleSt })).states as any[]) ||
      [];
    const entry: any = states.find((s) => s.id === child);
    expect(entry).toBeTruthy();
    // Root-level states carry no parent_id and sit at depth 0.
    expect(entry.parent_id).toBeUndefined();
    expect(entry.depth).toEqual(0);
  });

  test("test_duplicate_state", async () => {
    const sid = await addState(ctx.mcp, sampleSt, "S_DupSource");
    const result: any = await ctx.mcp.expect("statetree_state_duplicate", {
      asset_path: sampleSt,
      state: sid,
      new_name: "S_DupCopy",
    });
    const newId = result.state_id;
    expect(newId && newId !== sid).toBeTruthy();
    expect(result.guid_mapping && typeof result.guid_mapping === "object").toBeTruthy();
    const states =
      ((await ctx.mcp.expect("statetree_state_list", { asset_path: sampleSt })).states as any[]) ||
      [];
    const names = new Set(states.map((s) => s.name));
    expect(names.has("S_DupCopy")).toBe(true);
  });

  test("test_remove_state", async () => {
    const sid = await addState(ctx.mcp, sampleSt, "S_ToRemove");
    const result: any = await ctx.mcp.expect("statetree_state_remove", {
      asset_path: sampleSt,
      state: sid,
    });
    expect((result.removed_count ?? 0) >= 1).toBeTruthy();
    await assertReady(ctx.mcp);
    const states =
      ((await ctx.mcp.expect("statetree_state_list", { asset_path: sampleSt })).states as any[]) ||
      [];
    expect(new Set(states.map((s) => s.id)).has(sid)).toBe(false);
  });

  // ── transitions ────────────────────────────────────────────────────────────
  test("test_transition_lifecycle", async () => {
    const src = await addState(ctx.mcp, sampleSt, "S_TransSrc");
    const dst = await addState(ctx.mcp, sampleSt, "S_TransDst");
    // Add a GotoState transition (target referenced by GUID).
    const added: any = await ctx.mcp.expect("statetree_transition_add", {
      asset_path: sampleSt,
      state: src,
      trigger: "OnStateCompleted",
      target: dst,
      priority: "Normal",
    });
    const tid = added.transition_id;
    expect(tid).toBeTruthy();

    // Mutate it.
    await ctx.mcp.expect("statetree_transition_set_properties", {
      asset_path: sampleSt,
      transition_id: tid,
      priority: "High",
      trigger: "OnTick",
    });

    // Read back and confirm the mutated transition is attached to the source.
    const read: any = await ctx.mcp.expect("statetree_read", {
      asset_path: sampleSt,
      state_id: src,
    });
    const srcEntry: any = ((read.states as any[]) || []).find((s) => s.id === src);
    expect(srcEntry).toBeTruthy();
    const trans: any = ((srcEntry.transitions as any[]) || []).find((t) => t.id === tid);
    expect(trans).toBeTruthy();
    expect(trans.priority).toEqual("High");
    expect(trans.trigger).toEqual("OnTick");

    // Remove it.
    const removed: any = await ctx.mcp.expect("statetree_transition_remove", {
      asset_path: sampleSt,
      transition_id: tid,
    });
    expect(removed.success).toBe(true);
    const read2: any = await ctx.mcp.expect("statetree_read", {
      asset_path: sampleSt,
      state_id: src,
    });
    const srcEntry2: any = ((read2.states as any[]) || []).find((s) => s.id === src);
    expect(JSON.stringify(srcEntry2.transitions)).not.toContain(tid);
  });

  // ── nodes ──────────────────────────────────────────────────────────────────
  test("test_node_lifecycle", async () => {
    const nodeType = await firstTaskType(ctx.mcp);
    if (!nodeType) {
      console.log("SKIP editor exposes no StateTree task node types");
      return;
    }
    const sid = await addState(ctx.mcp, sampleSt, "S_NodeHost");
    const nid = await addTask(ctx.mcp, sampleSt, sid, nodeType);

    const props: any = await ctx.mcp.expect("statetree_node_get_properties", {
      asset_path: sampleSt,
      node_id: nid,
    });
    expect(props.node_id).toEqual(nid);
    expect(props.instance_data).toBeDefined();

    const bindable: any = await ctx.mcp.expect("statetree_binding_list_bindable", {
      asset_path: sampleSt,
      node_id: nid,
    });
    expect(bindable.node_properties).toBeDefined();
    expect(bindable.available_sources).toBeDefined();

    const removed: any = await ctx.mcp.expect("statetree_node_remove", {
      asset_path: sampleSt,
      node_id: nid,
    });
    expect(removed.success).toBe(true);
    await assertReady(ctx.mcp);
    // The node should no longer resolve.
    const gone = await ctx.mcp.command("statetree_node_get_properties", {
      asset_path: sampleSt,
      node_id: nid,
    });
    expect(gone.status).toEqual("error");
  });

  test("test_set_node_property", async () => {
    // A scalar-bearing task type (e.g. StateTreeDelayTask.Duration) — the
    // generic first-type pick is often StateTreeBlueprintTaskWrapper, whose
    // instance data has no scalar, which used to silently skip this test.
    const nodeType = await scalarTaskType(ctx.mcp);
    if (!nodeType) {
      console.log("SKIP editor exposes no StateTree task type with a scalar property");
      return;
    }
    const sid = await addState(ctx.mcp, sampleSt, "S_SetNodeProp");
    const nid = await addTask(ctx.mcp, sampleSt, sid, nodeType);

    const props: any = await ctx.mcp.expect("statetree_node_get_properties", {
      asset_path: sampleSt,
      node_id: nid,
    });
    const instance = (props.instance_data as any) || {};
    const chosen = settableProperty((instance.properties as any[]) || []);
    expect(chosen).toBeTruthy();
    const [name, value, matches] = chosen!;
    const result: any = await ctx.mcp.expect("statetree_node_set_property", {
      asset_path: sampleSt,
      node_id: nid,
      property_name: name,
      property_value: value,
    });
    expect(result.success).toBe(true);
    expect(result.property_name).toEqual(name);
    // Independent readback via statetree_node_get_properties (the pattern the
    // node-lifecycle test above already uses): the instance-data property must
    // now serialize the distinctive written value, not its default.
    const reread: any = await ctx.mcp.expect("statetree_node_get_properties", {
      asset_path: sampleSt,
      node_id: nid,
    });
    const entry = (((reread.instance_data as any) || {}).properties as any[])?.find(
      (p: any) => p.name === name,
    );
    expect(entry).toBeTruthy();
    expect(matches(entry.value)).toBe(true);
  });

  // ── property bindings ──────────────────────────────────────────────────────
  test("test_property_binding_lifecycle", async () => {
    const nodeType = await firstTaskType(ctx.mcp);
    if (!nodeType) {
      console.log("SKIP editor exposes no StateTree task node types");
      return;
    }
    const sid = await addState(ctx.mcp, sampleSt, "S_BindHost");
    const srcNode = await addTask(ctx.mcp, sampleSt, sid, nodeType);
    const dstNode = await addTask(ctx.mcp, sampleSt, sid, nodeType);

    await ctx.mcp.expect("statetree_binding_add", {
      asset_path: sampleSt,
      source_node_id: srcNode,
      source_property: "Output",
      target_node_id: dstNode,
      target_property: "MCPBoundValue",
    });

    const listed: any = await ctx.mcp.expect("statetree_binding_list", { asset_path: sampleSt });
    const bindings = (listed.bindings as any[]) || [];
    const match = bindings.find(
      (b) =>
        b.target_node_id === dstNode && String(b.target_property).includes("MCPBoundValue"),
    );
    expect(match).toBeTruthy();

    await ctx.mcp.expect("statetree_binding_remove", {
      asset_path: sampleSt,
      target_node_id: dstNode,
      target_property: "MCPBoundValue",
    });
    const listed2: any = await ctx.mcp.expect("statetree_binding_list", { asset_path: sampleSt });
    const still = ((listed2.bindings as any[]) || []).filter(
      (b) =>
        b.target_node_id === dstNode && String(b.target_property).includes("MCPBoundValue"),
    );
    expect(still.length).toBe(0);
  });

  // ── compile / verify ───────────────────────────────────────────────────────
  test("test_compile_verify_and_save", async () => {
    // statetree_compile is observed through statetree_verify (an independent,
    // non-mutating diagnostic): after mutation the tree verifies STALE, after
    // compile it verifies OK with hash_matches true and the stored hash equal
    // to the compile's stamped editor_data_hash. Note (verified live):
    // ready_to_run does NOT discriminate — it stays true even when stale,
    // because the previously-linked compiled data remains loaded; hash_matches
    // / status is the compile's real observable.
    const path = `${NS}/ST_Compile`;
    await ensureAbsent(ctx.mcp, path);
    await ctx.mcp.expect("statetree_create", { asset_path: path, schema_class: await pickSchema(ctx.mcp) });
    const sid = await addState(ctx.mcp, path, "S_Compiled");

    // Before: the state-add diverged the author-time hash from the compiled stamp.
    const before: any = await ctx.mcp.expect("statetree_verify", { asset_path: path });
    expect(before.hash_matches).not.toBe(true);
    expect(["stale", "never_compiled"]).toContain(before.status);

    const compiled: any = await ctx.mcp.expect("statetree_compile", {
      asset_path: path,
      auto_save: false,
    });
    expect(compiled.success).toBe(true);
    expect(compiled.compilation_status).toEqual("success");
    expect(compiled.editor_data_hash).toBeDefined();
    await assertReady(ctx.mcp);

    // The author-time read reflects the state regardless of compile outcome.
    const read: any = await ctx.mcp.expect("statetree_read", { asset_path: path });
    expect(JSON.stringify(read.states)).toContain(sid);

    // After: the independent diagnostic reports the compile landed.
    const verified: any = await ctx.mcp.expect("statetree_verify", { asset_path: path });
    expect(verified.status).toEqual("ok");
    expect(verified.hash_matches).toBe(true);
    expect(verified.ready_to_run).toBe(true);
    expect(verified.stored_hash).toEqual(compiled.editor_data_hash);

    const saved: any = await ctx.mcp.expect("statetree_save", { asset_path: path });
    expect(saved.success).toBe(true);
  });
});
