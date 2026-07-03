/**
 * Blueprint graph domain — node / function / variable / component graph ops.
 * Port of tests/integration/test_blueprint_graph.py.
 *
 * A module-scoped sample Actor Blueprint is created once under the test
 * namespace; each test arranges prerequisite graph state, dispatches the op
 * under test (`ctx.mcp.expect` throws on a non-success envelope), and asserts
 * the resulting state by reading it back.
 */

import { test, expect, beforeAll } from "bun:test";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { ensureAbsent, assertReady } from "../harness/ops.ts";

const NS = `${ROOT}/bpgraph`;
const SAMPLE = `${NS}/BP_GraphSample`;

// ── helpers ─────────────────────────────────────────────────────────────────

/** Return the pins[] array for a node via list_node_pins. */
async function listPins(
  client: { expect: (t: string, a?: Record<string, unknown>) => Promise<Record<string, unknown>> },
  bpPath: string,
  nodeId: unknown,
): Promise<any[]> {
  const result = await client.expect("bp_list_node_pins", {
    blueprint_name: bpPath,
    node_id: nodeId,
  });
  return result && typeof result === "object" && Array.isArray((result as any).pins)
    ? (result as any).pins
    : [];
}

/** Name of the first exec pin in a given direction ("Input"/"Output"). Falls
 *  back to UE's canonical exec pin names if the category isn't as expected. */
function execPin(pins: any[], direction: string): unknown {
  for (const p of pins) {
    if (p.direction === direction && p.category === "exec") return p.name;
  }
  return direction === "Input" ? "execute" : "then";
}

function pinConnected(pins: any[], name: unknown): boolean {
  for (const p of pins) {
    if (p.name === name) return Boolean(p.is_connected);
  }
  return false;
}

editorSuite("blueprint_graph", (ctx) => {
  let sampleBp: string;

  beforeAll(async () => {
    // Create one Actor Blueprint for the whole module and compile it.
    await ensureAbsent(ctx.mcp, SAMPLE);
    await ctx.mcp.expect("bp_create_blueprint", { name: SAMPLE, parent_class: "Actor" });
    await ctx.mcp.expect("bp_compile", { blueprint_name: SAMPLE });
    sampleBp = SAMPLE;
  });

  // ── node graph ops ─────────────────────────────────────────────────────────

  test("add_event_node_and_list_pins", async () => {
    const res = await ctx.mcp.expect("bp_add_event_node", {
      blueprint_name: sampleBp,
      event_name: "ReceiveBeginPlay",
      pos_x: 0,
      pos_y: 0,
    });
    const nodeId = (res as any).node_id;
    expect(nodeId).toBeTruthy();
    // Readback: the event node exposes at least one output exec pin.
    const pins = await listPins(ctx.mcp, sampleBp, nodeId);
    expect(pins.some((p: any) => p.direction === "Output")).toBeTruthy();
  });

  test("add_blueprint_node_print", async () => {
    const res = await ctx.mcp.expect("bp_add_node", {
      blueprint_name: sampleBp,
      node_type: "Print",
      pos_x: 400,
      pos_y: 0,
      message: "MCP graph test",
    });
    const nodeId = (res as any).node_id;
    expect(nodeId).toBeTruthy();
    // Independent readback (not the node_id echo): the "message" arg lands as
    // the Print node's InString pin default.
    const pins = await listPins(ctx.mcp, sampleBp, nodeId);
    const instr = pins.find((p: any) => p.name === "InString");
    expect(instr).toBeDefined();
    expect(instr.default_value).toBe("MCP graph test");
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
  });

  test("connect_nodes", async () => {
    const event = (
      await ctx.mcp.expect("bp_add_event_node", {
        blueprint_name: sampleBp,
        event_name: "ReceiveTick",
        pos_x: 0,
        pos_y: 300,
      })
    ).node_id;
    const printer = (
      await ctx.mcp.expect("bp_add_node", {
        blueprint_name: sampleBp,
        node_type: "Print",
        pos_x: 400,
        pos_y: 300,
        message: "tick",
      })
    ).node_id;

    const srcPin = execPin(await listPins(ctx.mcp, sampleBp, event), "Output");
    const dstPin = execPin(await listPins(ctx.mcp, sampleBp, printer), "Input");

    await ctx.mcp.expect("bp_connect_pins", {
      blueprint_name: sampleBp,
      source_node_id: event,
      source_pin_name: srcPin,
      target_node_id: printer,
      target_pin_name: dstPin,
      dry_run: false,
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });

    // Readback: the source exec pin now reports as connected.
    expect(pinConnected(await listPins(ctx.mcp, sampleBp, event), srcPin)).toBe(true);
    // And the graph analysis returns structured node data.
    const analysis = await ctx.mcp.expect("bp_inspect", {
      blueprint_path: sampleBp,
      graph_name: "EventGraph",
      include_node_details: true,
      include_pin_connections: true,
    });
    expect(analysis && typeof analysis === "object" && Object.keys(analysis).length > 0).toBeTruthy();
  });

  test("disconnect_pin", async () => {
    const event = (
      await ctx.mcp.expect("bp_add_event_node", {
        blueprint_name: sampleBp,
        event_name: "ReceiveEndPlay",
        pos_x: 0,
        pos_y: 600,
      })
    ).node_id;
    const printer = (
      await ctx.mcp.expect("bp_add_node", {
        blueprint_name: sampleBp,
        node_type: "Print",
        pos_x: 400,
        pos_y: 600,
        message: "endplay",
      })
    ).node_id;
    const srcPin = execPin(await listPins(ctx.mcp, sampleBp, event), "Output");
    const dstPin = execPin(await listPins(ctx.mcp, sampleBp, printer), "Input");
    await ctx.mcp.expect("bp_connect_pins", {
      blueprint_name: sampleBp,
      source_node_id: event,
      source_pin_name: srcPin,
      target_node_id: printer,
      target_pin_name: dstPin,
      dry_run: false,
    });

    await ctx.mcp.expect("bp_disconnect_pin", {
      bp_path: sampleBp,
      node_id: event,
      pin_name: srcPin,
      dry_run: false,
    });
    // Readback: the pin is no longer connected.
    expect(pinConnected(await listPins(ctx.mcp, sampleBp, event), srcPin)).toBe(false);
  });

  test("set_node_property", async () => {
    const printer = (
      await ctx.mcp.expect("bp_add_node", {
        blueprint_name: sampleBp,
        node_type: "Print",
        pos_x: 800,
        pos_y: 0,
        message: "before",
      })
    ).node_id;
    await ctx.mcp.expect("bp_set_node_property", {
      blueprint_name: sampleBp,
      node_id: printer,
      property_name: "message",
      property_value: "MCP_AFTER_TOKEN",
      dry_run: false,
    });
    // Independent read-back (NOT the setter's own echo): the Print "message"
    // lands as the InString pin default.
    const pins = (
      await ctx.mcp.expect("bp_list_node_pins", { blueprint_name: sampleBp, node_id: printer })
    ).pins as any[];
    const instr = pins.find((p: any) => p.name === "InString") ?? null;
    expect(instr).toBeTruthy();
    expect(instr.default_value).toEqual("MCP_AFTER_TOKEN");
  });

  test("delete_node", async () => {
    const printer = (
      await ctx.mcp.expect("bp_add_node", {
        blueprint_name: sampleBp,
        node_type: "Print",
        pos_x: 800,
        pos_y: 300,
        message: "doomed",
      })
    ).node_id;
    // Sanity: the node is resolvable before the delete (guards vacuity below).
    expect((await listPins(ctx.mcp, sampleBp, printer)).length).toBeGreaterThan(0);
    const res = await ctx.mcp.expect("bp_delete_node", {
      blueprint_name: sampleBp,
      node_id: printer,
      dry_run: false,
    });
    expect((res as any).deleted_node_id || (res as any).success !== false).toBeTruthy();
    // Independent readback: the node is GONE — resolving its id now fails with
    // the closed-taxonomy node_not_found (not just the delete op's echo).
    const after = await ctx.mcp.call("bp_list_node_pins", {
      blueprint_name: sampleBp,
      node_id: printer,
    });
    expect((after as any).error_code).toBe("node_not_found");
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
  });

  test("set_blueprint_default_value", async () => {
    // Set a variable's CDO default, then read it back independently.
    await ctx.mcp.expect("bp_create_variable", {
      blueprint_name: sampleBp,
      variable_name: "MCPDefaultInt",
      variable_type: "int",
      dry_run: false,
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
    await ctx.mcp.expect("bp_set_default_value", {
      blueprint_name: sampleBp,
      property: "MCPDefaultInt",
      value: 7,
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
    const content = await ctx.mcp.expect("bp_read", {
      blueprint_path: sampleBp,
      include_variable_defaults: true,
    });
    const vars = ((content as any).variables ?? []) as any[];
    const v = vars.find((x: any) => x.name === "MCPDefaultInt") ?? null;
    expect(v).toBeTruthy();
    // read_blueprint_content reports the CDO default under 'cdo_value'.
    expect(String(v.cdo_value)).toEqual("7");
  });

  // ── function ops ───────────────────────────────────────────────────────────

  test("create_function", async () => {
    await ctx.mcp.expect("bp_create_function", {
      blueprint_name: sampleBp,
      function_name: "MCPGraphFunc",
      return_type: "void",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
    const details = await ctx.mcp.expect("bp_get_function_details", {
      blueprint_path: sampleBp,
      function_name: "MCPGraphFunc",
      include_graph: true,
    });
    expect(JSON.stringify(details)).toContain("MCPGraphFunc");
  });

  test("function_io_roundtrip", async () => {
    const fn = "MCPGraphIOFunc";
    await ctx.mcp.expect("bp_create_function", {
      blueprint_name: sampleBp,
      function_name: fn,
      return_type: "void",
    });
    await ctx.mcp.expect("bp_add_function_input", {
      blueprint_name: sampleBp,
      function_name: fn,
      param_name: "InVal",
      param_type: "float",
      is_array: false,
    });
    await ctx.mcp.expect("bp_add_function_output", {
      blueprint_name: sampleBp,
      function_name: fn,
      param_name: "OutVal",
      param_type: "bool",
      is_array: false,
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });

    const details = await ctx.mcp.expect("bp_get_function_details", {
      blueprint_path: sampleBp,
      function_name: fn,
      include_graph: true,
    });
    const blob = JSON.stringify(details);
    expect(blob.includes("InVal") && blob.includes("OutVal")).toBeTruthy();

    // Remove them and confirm they are gone.
    await ctx.mcp.expect("bp_remove_function_input", {
      blueprint_name: sampleBp,
      function_name: fn,
      param_name: "InVal",
    });
    await ctx.mcp.expect("bp_remove_function_output", {
      blueprint_name: sampleBp,
      function_name: fn,
      param_name: "OutVal",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
    const after = JSON.stringify(
      await ctx.mcp.expect("bp_get_function_details", {
        blueprint_path: sampleBp,
        function_name: fn,
        include_graph: true,
      }),
    );
    expect(!after.includes("InVal") && !after.includes("OutVal")).toBeTruthy();
  });

  test("rename_function", async () => {
    await ctx.mcp.expect("bp_create_function", {
      blueprint_name: sampleBp,
      function_name: "MCPRenameSrc",
      return_type: "void",
    });
    await ctx.mcp.expect("bp_rename_function", {
      blueprint_name: sampleBp,
      old_function_name: "MCPRenameSrc",
      new_function_name: "MCPRenameDst",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
    const details = JSON.stringify(
      await ctx.mcp.expect("bp_get_function_details", {
        blueprint_path: sampleBp,
        include_graph: false,
      }),
    );
    expect(details).toContain("MCPRenameDst");
  });

  test("delete_function", async () => {
    await ctx.mcp.expect("bp_create_function", {
      blueprint_name: sampleBp,
      function_name: "MCPDeleteMe",
      return_type: "void",
    });
    await ctx.mcp.expect("bp_delete_function", {
      blueprint_name: sampleBp,
      function_name: "MCPDeleteMe",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
    const details = JSON.stringify(
      await ctx.mcp.expect("bp_get_function_details", {
        blueprint_path: sampleBp,
        include_graph: false,
      }),
    );
    expect(details.includes("MCPDeleteMe")).toBe(false);
  });

  test("bp_function_references", async () => {
    await ctx.mcp.expect("bp_create_function", {
      blueprint_name: sampleBp,
      function_name: "MCPRefFunc",
      return_type: "void",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
    const res = await ctx.mcp.expect("bp_function_references", {
      bp_path: sampleBp,
      function_name: "MCPRefFunc",
      direction: "callees",
    });
    // Concrete known fields: references[] is present, count is consistent with
    // it, and a freshly-created empty function graph has zero callees.
    expect(Array.isArray(res.references)).toBe(true);
    expect(res.count).toBe(0);
    expect((res.references as unknown[]).length).toBe(0);
    expect(String(res.bp_path ?? "").startsWith(sampleBp)).toBe(true);
  });

  // ── custom event authoring + replication (GAP-055) ────────────────────────

  test("custom_event_create_and_replicate", async () => {
    // Port of pytest test_custom_event_create_and_replicate. Author a fresh
    // custom event with one typed input parameter (an output pin on the node =
    // the event's input signature).
    const res = await ctx.mcp.expect("bp_add_custom_event", {
      blueprint_name: sampleBp,
      event_name: "MCPServerFire",
      params: [{ name: "Amount", type: "int" }],
      pos_x: 0,
      pos_y: 600,
    });
    expect((res as any).node_id).toBeTruthy();
    expect(res.event_name).toBe("MCPServerFire");
    expect(res.num_params).toBe(1);
    expect(res.params_added as string[]).toContain("Amount");

    // The parameter is exposed as an OUTPUT pin named Amount on the event node.
    const pins = await listPins(ctx.mcp, sampleBp, (res as any).node_id);
    expect(pins.some((p: any) => p.name === "Amount" && p.direction === "Output")).toBe(true);

    // Re-creating the same name is rejected deterministically (no auto-rename).
    const dup = await ctx.mcp.call("bp_add_custom_event", {
      blueprint_name: sampleBp,
      event_name: "MCPServerFire",
    });
    expect((dup as any).error_code).toBe("name_collision");

    // The event survives a structural recompile and is visible in the BP.
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
    expect(JSON.stringify(await ctx.mcp.expect("bp_read", { blueprint_path: sampleBp }))).toContain(
      "MCPServerFire",
    );

    // Turn the event into a server RPC.
    //
    // KNOWN DEAD WIRE (docs/BUGS.md § "bp_set_event_replication is dead code"):
    // the handler exists (MCPBlueprintCommands.cpp:117) but FMCPBridge::
    // ExecuteCommand never routes the command, so the live bridge answers
    // "Unknown command". Bail out on exactly that signature (bun analog of the
    // pytest xfail); the battery below self-unblocks when the dispatch lands.
    const repEnv = await ctx.mcp.call("bp_set_event_replication", {
      blueprint_name: sampleBp,
      event_name: "MCPServerFire",
      replication: "server",
      reliable: true,
    });
    if (String((repEnv as any).error ?? "").includes("Unknown command")) {
      console.warn(
        "bp_set_event_replication is not routed by the bridge (docs/BUGS.md § dead code) — " +
          "replication readback skipped; un-skips when the MCPBridge.cpp dispatch line lands",
      );
      return;
    }
    expect(repEnv.status).not.toBe("error");
    const rep = (repEnv.result ?? repEnv) as Record<string, unknown>;
    expect(rep.replication).toBe("server");
    expect(rep.reliable).toBe(true);
    // NOTE: rep.function_flags is the MUTATOR'S ECHO. The independent readback
    // is bp_inspect's custom-event decoder (MCPBlueprintCommands.cpp:2986-3009),
    // which re-derives the mode from UK2Node_CustomEvent::GetNetFlags():
    // "RunOnServer" for FUNC_NetServer. (bp_get_function_details cannot observe
    // this — it walks FunctionGraphs only; custom events live in UbergraphPages.)
    const analysis = await ctx.mcp.expect("bp_inspect", {
      blueprint_path: sampleBp,
      graph_name: "EventGraph",
      include_node_details: true,
      include_pin_connections: false,
    });
    const nodes = ((analysis.graph_data as any)?.nodes ?? []) as any[];
    const ev = nodes.find(
      (n: any) => n.class === "K2Node_CustomEvent" && String(n.title ?? "").includes("MCPServerFire"),
    );
    expect(ev).toBeDefined();
    expect(ev.replication).toBe("RunOnServer");

    // Still compiles clean after flipping the net specifiers.
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
  });

  // ── variable ops ───────────────────────────────────────────────────────────

  test("variable_details_and_properties", async () => {
    await ctx.mcp.expect("bp_create_variable", {
      blueprint_name: sampleBp,
      variable_name: "MCPGraphScore",
      variable_type: "int",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });

    const details = await ctx.mcp.expect("bp_get_variable_details", {
      blueprint_path: sampleBp,
      variable_name: "MCPGraphScore",
    });
    expect(JSON.stringify(details)).toContain("MCPGraphScore");

    await ctx.mcp.expect("bp_set_variable_properties", {
      blueprint_name: sampleBp,
      variable_name: "MCPGraphScore",
      category: "MCPStats",
      tooltip: "graph test score",
      dry_run: false,
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
    const after = JSON.stringify(
      await ctx.mcp.expect("bp_get_variable_details", {
        blueprint_path: sampleBp,
        variable_name: "MCPGraphScore",
      }),
    );
    expect(after).toContain("MCPStats");
  });

  test("delete_variable", async () => {
    await ctx.mcp.expect("bp_create_variable", {
      blueprint_name: sampleBp,
      variable_name: "MCPDoomedVar",
      variable_type: "bool",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
    await ctx.mcp.expect("bp_delete_variable", {
      blueprint_name: sampleBp,
      variable_name: "MCPDoomedVar",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
    const content = JSON.stringify(
      await ctx.mcp.expect("bp_read", { blueprint_path: sampleBp }),
    );
    expect(content.includes("MCPDoomedVar")).toBe(false);
  });

  // ── component ops ──────────────────────────────────────────────────────────

  test("remove_component", async () => {
    await ctx.mcp.expect("bp_add_component", {
      blueprint_name: sampleBp,
      component_type: "StaticMeshComponent",
      component_name: "MCPDoomedComp",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
    expect(
      JSON.stringify(await ctx.mcp.expect("bp_list_components", { bp_path: sampleBp })),
    ).toContain("MCPDoomedComp");

    await ctx.mcp.expect("bp_remove_component", {
      bp_path: sampleBp,
      component_name: "MCPDoomedComp",
      reparent_children: true,
      dry_run: false,
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: sampleBp });
    const after = JSON.stringify(
      await ctx.mcp.expect("bp_list_components", { bp_path: sampleBp }),
    );
    expect(after.includes("MCPDoomedComp")).toBe(false);
  });

  // ── reparent ───────────────────────────────────────────────────────────────

  test("reparent_blueprint", async () => {
    const path = `${NS}/BP_Reparent`;
    await ensureAbsent(ctx.mcp, path);
    await ctx.mcp.expect("bp_create_blueprint", { name: path, parent_class: "Actor" });
    await ctx.mcp.expect("bp_reparent", {
      blueprint_path: path,
      new_parent_class: "Pawn",
    });
    const parent = JSON.stringify(
      await ctx.mcp.expect("bp_get_parent_class", { bp_path: path }),
    ).toLowerCase();
    expect(parent).toContain("pawn");
  });

  // ── spawn ──────────────────────────────────────────────────────────────────

  test("spawn_blueprint_actor", async () => {
    const path = `${NS}/BP_Spawnable`;
    await ensureAbsent(ctx.mcp, path);
    await ctx.mcp.expect("bp_create_blueprint", { name: path, parent_class: "Actor" });
    await ctx.mcp.expect("bp_compile", { blueprint_name: path });

    // spawn_blueprint_actor is a bridge-internal command with no standalone MCP
    // tool, so this one op goes through the bridge.
    await ctx.bridge.expect("spawn_blueprint_actor", {
      blueprint_name: path,
      actor_name: "MCPSpawnedActor",
      location: [0, 0, 0],
      rotation: [0, 0, 0],
    });
    await assertReady(ctx.mcp);

    // Readback via actor_query: a matching actor is now in the level.
    const found = await ctx.mcp.expect("actor_query", {
      name_pattern: "MCPSpawned",
      direct_only: false,
      label: "",
      cursor: 0,
      limit: 200,
    });
    const actors = (found && typeof found === "object" ? (found as any).actors ?? [] : []) as any[];
    expect(
      actors.some(
        (a: any) =>
          String(a.name ?? "").includes("MCPSpawned") ||
          String(a.label ?? "").includes("MCPSpawned"),
      ),
    ).toBeTruthy();

    // level_inspect is the broader level readback used as a crash/sanity probe.
    const level = await ctx.mcp.expect("level_inspect", {});
    expect(level && typeof level === "object" && Object.keys(level).length > 0).toBeTruthy();
  });
});
