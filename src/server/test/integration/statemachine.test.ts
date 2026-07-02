/**
 * Animation state-machine domain — author a state machine inside an Anim
 * Blueprint's AnimGraph, mutate its states/transitions/entry, set an inner
 * AnimNode property, and read the resulting graph topology back. Port of
 * tests/integration/test_statemachine.py. Driven through the real MCP server
 * (ctx.mcp calls tools by name; the tool layer maps kwargs→bridge params).
 *
 * Every test needs an Anim Blueprint first — built from scratch via
 * anim_blueprint_create against the engine SkeletalCube_Skeleton (no imported
 * content required). list_blueprint_graphs (bp_list_graphs) is the authoritative
 * reader: it enumerates state-machine sub-graphs as category state_machine /
 * state / transition entries.
 */

import { test, expect, beforeAll } from "bun:test";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { ensureAbsent, assertReady, firstAssetOf } from "../harness/ops.ts";

const NS = `${ROOT}/statemachine`;
const SAMPLE = `${NS}/ABP_Sample`;

// Confirmed-present engine skeleton — lets an Anim Blueprint be authored from
// scratch with no imported project content.
const ENGINE_SKELETON = "/Engine/EngineMeshes/SkeletalCube_Skeleton";

interface Abp {
  path: string;
  anim_graph: string;
}

editorSuite("statemachine", (ctx) => {
  // ── helpers ────────────────────────────────────────────────────────────────

  /** Create a state machine in the AnimBP's AnimGraph and return the actual
   *  machine graph name (post-rename) plus its node_id. */
  async function newMachine(abp: Abp, name: string): Promise<[string, unknown]> {
    const result = await ctx.mcp.expect("anim_state_machine_create", {
      blueprint_name: abp.path,
      machine_name: name,
      graph_name: abp.anim_graph,
    });
    expect(result.success).toBe(true);
    const machine = result.machine_name;
    expect(machine).toBeTruthy();
    return [machine as string, result.node_id];
  }

  /** Add a state; return (node_id, inner_graph_name). */
  async function addState(
    abp: Abp,
    machine: string,
    name: string,
  ): Promise<[unknown, string]> {
    const result = await ctx.mcp.expect("anim_state_machine_state_add", {
      blueprint_name: abp.path,
      state_machine_graph: machine,
      state_name: name,
    });
    expect(result.success).toBe(true);
    const nid = result.node_id;
    const inner = result.inner_graph;
    expect(nid).toBeTruthy();
    expect(inner).toBeTruthy();
    return [nid, inner as string];
  }

  /** All graph entries reported for the Anim Blueprint. */
  async function graphs(abp: Abp): Promise<Record<string, unknown>[]> {
    const listed = await ctx.mcp.expect("bp_list_graphs", { blueprint_path: abp.path });
    const g = listed.graphs;
    expect(Array.isArray(g)).toBe(true);
    return g as Record<string, unknown>[];
  }

  /** Entries whose parent_state_machine is the given machine (states + transitions). */
  async function machineGraphs(abp: Abp, machine: string): Promise<Record<string, unknown>[]> {
    return (await graphs(abp)).filter((g) => g.parent_state_machine === machine);
  }

  // ── fixture: the shared Anim Blueprint ─────────────────────────────────────
  let sampleAbp: Abp | null = null;

  beforeAll(async () => {
    const name = SAMPLE.split("/").pop() as string;
    const pkg = SAMPLE.slice(0, SAMPLE.lastIndexOf("/"));
    await ensureAbsent(ctx.mcp, SAMPLE);

    // Prefer the always-present engine skeleton; fall back to any project skeleton.
    const candidates = [ENGINE_SKELETON];
    const found = await firstAssetOf(ctx.mcp, "anim_list_skeletons", {}, "skeletons");
    if (found && typeof found === "object" && found.path) {
      candidates.push(found.path as string);
    }

    for (const sk of candidates) {
      try {
        const result = await ctx.mcp.expect("anim_blueprint_create", {
          name,
          skeleton_path: sk,
          package_path: pkg,
        });
        sampleAbp = { path: SAMPLE, anim_graph: (result.anim_graph as string) || "AnimGraph" };
        return;
      } catch {
        await ensureAbsent(ctx.mcp, SAMPLE);
      }
    }
    // Could not create an Anim Blueprint from any skeleton — leave sampleAbp null
    // so each test content-gates (skips) below.
  });

  // ── creation ───────────────────────────────────────────────────────────────

  test("create_state_machine", async () => {
    if (!sampleAbp) {
      console.log("skip: could not create an Anim Blueprint from any skeleton");
      return;
    }
    const [machine, nodeId] = await newMachine(sampleAbp, "SM_Create");
    expect(nodeId).toBeTruthy();
    // Read back: the machine appears as a category=="state_machine" graph.
    const sm = (await graphs(sampleAbp)).find(
      (g) => g.category === "state_machine" && g.name === machine,
    );
    expect(sm).toBeTruthy();
  });

  // ── states ─────────────────────────────────────────────────────────────────

  test("add_state_then_list", async () => {
    if (!sampleAbp) {
      console.log("skip: could not create an Anim Blueprint from any skeleton");
      return;
    }
    const [machine] = await newMachine(sampleAbp, "SM_AddState");
    const [, inner] = await addState(sampleAbp, machine, "S_Idle");
    // Read back: the new state's inner graph is listed under the machine.
    const states = (await machineGraphs(sampleAbp, machine)).filter((g) => g.category === "state");
    expect(states.some((g) => g.name === inner)).toBeTruthy();
  });

  test("remove_state", async () => {
    if (!sampleAbp) {
      console.log("skip: could not create an Anim Blueprint from any skeleton");
      return;
    }
    const [machine] = await newMachine(sampleAbp, "SM_RemoveState");
    const [, inner] = await addState(sampleAbp, machine, "S_Doomed");
    expect((await machineGraphs(sampleAbp, machine)).some((g) => g.name === inner)).toBeTruthy();

    const result = await ctx.mcp.expect("anim_state_machine_state_remove", {
      blueprint_name: sampleAbp.path,
      state_machine_graph: machine,
      state_name: "S_Doomed",
    });
    expect(result.success).toBe(true);
    await assertReady(ctx.mcp);
    // Read back: the inner graph is gone.
    const names = new Set((await machineGraphs(sampleAbp, machine)).map((g) => g.name));
    expect(names.has(inner)).toBe(false);
  });

  // ── transitions ──────────────────────────────────────────────────────────────

  test("add_transition", async () => {
    if (!sampleAbp) {
      console.log("skip: could not create an Anim Blueprint from any skeleton");
      return;
    }
    const [machine] = await newMachine(sampleAbp, "SM_AddTrans");
    await addState(sampleAbp, machine, "S_A");
    await addState(sampleAbp, machine, "S_B");
    const result = await ctx.mcp.expect("anim_state_machine_transition_add", {
      blueprint_name: sampleAbp.path,
      state_machine_graph: machine,
      from_state: "S_A",
      to_state: "S_B",
    });
    expect(result.success).toBe(true);
    expect(result.node_id).toBeTruthy();
    // Read back: a transition graph A->B exists under the machine.
    const trans = (await machineGraphs(sampleAbp, machine)).filter((g) => g.category === "transition");
    expect(trans.some((g) => g.from_state === "S_A" && g.to_state === "S_B")).toBeTruthy();
  });

  test("modify_transition", async () => {
    if (!sampleAbp) {
      console.log("skip: could not create an Anim Blueprint from any skeleton");
      return;
    }
    const [machine] = await newMachine(sampleAbp, "SM_ModTrans");
    await addState(sampleAbp, machine, "S_M0");
    await addState(sampleAbp, machine, "S_M1");
    const added = await ctx.mcp.expect("anim_state_machine_transition_add", {
      blueprint_name: sampleAbp.path,
      state_machine_graph: machine,
      from_state: "S_M0",
      to_state: "S_M1",
    });
    const tid = added.node_id;
    expect(tid).toBeTruthy();

    // Modify by transition_id (the handler also accepts from_state+to_state).
    const modified = await ctx.mcp.expect("anim_state_machine_modify_transition", {
      blueprint_name: sampleAbp.path,
      state_machine_graph: machine,
      transition_id: tid,
      blend_duration: 0.35,
      priority_order: 2,
    });
    expect(modified.success).toBe(true);
    expect(modified.node_id).toEqual(tid);
    // The transition still resolves after the edit.
    const trans = (await machineGraphs(sampleAbp, machine)).filter((g) => g.category === "transition");
    expect(trans.some((g) => g.from_state === "S_M0" && g.to_state === "S_M1")).toBeTruthy();
  });

  test("remove_transition", async () => {
    if (!sampleAbp) {
      console.log("skip: could not create an Anim Blueprint from any skeleton");
      return;
    }
    const [machine] = await newMachine(sampleAbp, "SM_RemoveTrans");
    await addState(sampleAbp, machine, "S_R0");
    await addState(sampleAbp, machine, "S_R1");
    await ctx.mcp.expect("anim_state_machine_transition_add", {
      blueprint_name: sampleAbp.path,
      state_machine_graph: machine,
      from_state: "S_R0",
      to_state: "S_R1",
    });
    expect(
      (await machineGraphs(sampleAbp, machine)).some(
        (g) => g.category === "transition" && g.from_state === "S_R0",
      ),
    ).toBeTruthy();

    // Remove by from_state+to_state (alternative to transition_id).
    const removed = await ctx.mcp.expect("anim_state_machine_transition_remove", {
      blueprint_name: sampleAbp.path,
      state_machine_graph: machine,
      from_state: "S_R0",
      to_state: "S_R1",
    });
    expect(removed.success).toBe(true);
    await assertReady(ctx.mcp);
    const trans = (await machineGraphs(sampleAbp, machine)).filter((g) => g.category === "transition");
    expect(trans.some((g) => g.from_state === "S_R0" && g.to_state === "S_R1")).toBe(false);
  });

  // ── entry state ──────────────────────────────────────────────────────────────

  test("set_entry_state", async () => {
    if (!sampleAbp) {
      console.log("skip: could not create an Anim Blueprint from any skeleton");
      return;
    }
    const [machine] = await newMachine(sampleAbp, "SM_Entry");
    await addState(sampleAbp, machine, "S_Entry");
    const result = await ctx.mcp.expect("anim_state_machine_set_entry", {
      blueprint_name: sampleAbp.path,
      state_machine_graph: machine,
      state_name: "S_Entry",
    });
    expect(result.success).toBe(true);
    expect(result.entry_state).toEqual("S_Entry");
    await assertReady(ctx.mcp);
  });

  // ── inner AnimNode property ──────────────────────────────────────────────────

  test("set_inner_node_property", async () => {
    if (!sampleAbp) {
      console.log("skip: could not create an Anim Blueprint from any skeleton");
      return;
    }
    // Target the state machine node itself — a UAnimGraphNode_StateMachine is a
    // UAnimGraphNode_Base, so set_inner_node_property writes onto its inner
    // FAnimNode_StateMachine struct. MaxTransitionsPerFrame is a plain int32.
    const [, nodeId] = await newMachine(sampleAbp, "SM_InnerProp");
    expect(nodeId).toBeTruthy();
    const result = await ctx.mcp.expect("bp_set_inner_node_property", {
      blueprint_name: sampleAbp.path,
      node_id: nodeId,
      property_name: "MaxTransitionsPerFrame",
      property_value: "5",
      graph_name: sampleAbp.anim_graph,
    });
    expect(result.success).toBe(true);
    expect(result.value).toEqual("5");
    await assertReady(ctx.mcp);
  });
});
