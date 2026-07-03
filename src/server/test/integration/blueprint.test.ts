/**
 * Blueprint domain — create an asset, mutate its graph/components/variables,
 * read the state back. Port of tests/integration/test_blueprint.py.
 *
 * Self-contained: needs no imported content. A module-scoped sample Blueprint is
 * created once under the test namespace and reused.
 *
 * Pattern for every test: arrange prerequisite state -> dispatch the op (raises on
 * a non-success envelope) -> assert the resulting state via a read/inspect op.
 */

import { test, expect, beforeAll } from "bun:test";
import { existsSync, statSync } from "node:fs";
import { join } from "node:path";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { ensureAbsent } from "../harness/ops.ts";
import { projectDir } from "../harness/env.ts";

const NS = `${ROOT}/blueprint`;
const SAMPLE = `${NS}/BP_Sample`;

/** Map a /Game/... content path to its on-disk package file. The file only
 *  exists after the asset is SAVED. Mirrors config.uasset_disk_path. */
function uassetDiskPath(gamePath: string, ext = ".uasset"): string {
  const pkg = gamePath.split(".")[0] ?? "";
  if (!pkg.startsWith("/Game/")) throw new Error(`not a /Game/ path: ${gamePath}`);
  const rel = pkg.slice("/Game/".length);
  return join(projectDir(), "Content", rel + ext);
}

editorSuite("blueprint", (ctx) => {
  // module-scoped sample_bp fixture: create one Actor Blueprint and compile it.
  beforeAll(async () => {
    await ensureAbsent(ctx.mcp, SAMPLE);
    await ctx.mcp.expect("bp_create_blueprint", { name: SAMPLE, parent_class: "Actor" });
    await ctx.mcp.expect("bp_compile", { blueprint_name: SAMPLE });
  });

  test("create_blueprint_writes_uasset_on_disk", async () => {
    const path = `${NS}/BP_Created`;
    await ensureAbsent(ctx.mcp, path);
    const result = await ctx.mcp.expect("bp_create_blueprint", { name: path, parent_class: "Actor" });
    expect(result.success).toBe(true);
    // State assertion: save it, then the package file must exist at the mapped Content path.
    await ctx.mcp.expect("asset_save", { asset_paths: [path] });
    const disk = uassetDiskPath(path);
    expect(existsSync(disk) && statSync(disk).isFile()).toBe(true);
  });

  /** Independent readback for bp_compile: does the GENERATED class carry a
   *  UFunction of this name? The function subobject's path is
   *  `<pkg>.<Name>_C:<Function>` — it exists only after a real kismet compile
   *  (bp_add_custom_event marks the BP structurally modified, which regenerates
   *  the SKELETON class only). Observed via the sanctioned py console hatch. */
  async function generatedClassHasFunction(bpPath: string, fn: string): Promise<boolean> {
    const name = bpPath.split("/").pop();
    const probe = await ctx.mcp.expect("editor_console_exec", {
      command:
        "py import unreal; " +
        `f = unreal.find_object(None, '${bpPath}.${name}_C:${fn}'); ` +
        "print('MCPTEST_FN=' + ('present' if f else 'absent'))",
    });
    const m = /MCPTEST_FN=(\w+)/.exec(String(probe.output ?? ""));
    if (!m) throw new Error(`no probe marker: ${JSON.stringify(probe)}`);
    return m[1] === "present";
  }

  test("compile_blueprint", async () => {
    // bp_compile must actually recompile the generated class — not just return
    // success. A custom event added via bp_add_custom_event exists only on the
    // graph/skeleton; the generated class gains its UFunction only at compile.
    const path = `${NS}/BP_CompileProbe`;
    await ensureAbsent(ctx.mcp, path);
    try {
      await ctx.mcp.expect("bp_create_blueprint", { name: path, parent_class: "Actor" });
      await ctx.mcp.expect("bp_compile", { blueprint_name: path });
      await ctx.mcp.expect("bp_add_custom_event", {
        blueprint_name: path,
        event_name: "MCPCompileProbeEvent",
      });
      // Before the compile under test (proves the observation is not vacuous).
      expect(await generatedClassHasFunction(path, "MCPCompileProbeEvent")).toBe(false);

      const result = await ctx.mcp.expect("bp_compile", { blueprint_name: path });
      expect(result.compiled).toBe(true);

      // Independent readback: the generated class now exposes the UFunction.
      expect(await generatedClassHasFunction(path, "MCPCompileProbeEvent")).toBe(true);
    } finally {
      await ensureAbsent(ctx.mcp, path);
    }
  });

  test("bp_brief", async () => {
    const result = await ctx.mcp.expect("bp_brief", { bp_path: SAMPLE });
    // Concrete known fields (not just "non-empty dict"): parent class path
    // resolves to Actor and the ubergraph is listed by its real name.
    expect(String(result.parent_class ?? "")).toContain("Actor");
    const graphNames = ((result.graphs ?? []) as Array<{ name?: string }>).map((g) => g.name);
    expect(graphNames).toContain("EventGraph");
    expect(typeof result.variables_count).toBe("number");
  });

  test("bp_get_parent_class", async () => {
    const result = await ctx.mcp.expect("bp_get_parent_class", { bp_path: SAMPLE });
    // Parent was Actor; the reported parent class should mention it.
    const blob = JSON.stringify(result).toLowerCase();
    expect(blob).toContain("actor");
  });

  test("read_blueprint_content", async () => {
    const result = await ctx.mcp.expect("bp_read", { blueprint_path: SAMPLE });
    // Concrete known fields: the parent class and the asset's own name.
    expect(result.parent_class).toBe("Actor");
    expect(result.blueprint_name).toBe("BP_Sample");
  });

  test("list_blueprint_graphs", async () => {
    const result = await ctx.mcp.expect("bp_list_graphs", { blueprint_path: SAMPLE });
    // Concrete known fields: parent class + the ubergraph entry by name/category.
    expect(result.parent_class).toBe("Actor");
    const graphs = (result.graphs ?? []) as Array<{ name?: string; category?: string }>;
    const eventGraph = graphs.find((g) => g.name === "EventGraph");
    expect(eventGraph).toBeDefined();
    expect(eventGraph?.category).toBe("ubergraph");
  });

  test("add_component_then_list", async () => {
    await ctx.mcp.expect("bp_add_component", {
      blueprint_name: SAMPLE,
      component_type: "StaticMeshComponent",
      component_name: "MCPTestMesh",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: SAMPLE });
    const comps = await ctx.mcp.expect("bp_list_components", { bp_path: SAMPLE });
    expect(JSON.stringify(comps)).toContain("MCPTestMesh");
  });

  test("create_variable_then_read", async () => {
    await ctx.mcp.expect("bp_create_variable", {
      blueprint_name: SAMPLE,
      variable_name: "MCPTestHealth",
      variable_type: "float",
    });
    await ctx.mcp.expect("bp_compile", { blueprint_name: SAMPLE });
    const content = await ctx.mcp.expect("bp_read", { blueprint_path: SAMPLE });
    expect(JSON.stringify(content)).toContain("MCPTestHealth");
  });

  /** Independent readback: bp_read's archetype-diff override list for one SCS
   *  component template. Returns {property_name: override_entry}. */
  async function componentOverrides(
    bpPath: string,
    component: string,
  ): Promise<Record<string, Record<string, string>>> {
    const content = await ctx.mcp.expect("bp_read", {
      blueprint_path: bpPath,
      include_event_graph: false,
      include_functions: false,
      include_variables: false,
      include_component_properties: true,
    });
    const comps = content.components as Array<Record<string, unknown>>;
    const comp = comps.find((c) => c.name === component);
    if (!comp) throw new Error(`component ${component} not in bp_read of ${bpPath}`);
    const out: Record<string, Record<string, string>> = {};
    for (const o of (comp.property_overrides ?? []) as Array<Record<string, string>>) {
      out[o.name as string] = o;
    }
    return out;
  }

  test("set_component_property_then_read_back", async () => {
    // Write a component-template UPROPERTY, then prove the value landed via
    // bp_read's archetype-diff overrides (a different read primitive), not the
    // setter's echo.
    const path = `${NS}/BP_CompProp`;
    await ensureAbsent(ctx.mcp, path);
    try {
      await ctx.mcp.expect("bp_create_blueprint", { name: path, parent_class: "Actor" });
      await ctx.mcp.expect("bp_add_component", {
        blueprint_name: path,
        component_type: "StaticMeshComponent",
        component_name: "PropMesh",
      });
      await ctx.mcp.expect("bp_set_component_property", {
        blueprint_name: path,
        component_name: "PropMesh",
        property: "BoundsScale",
        value: 3.5,
      });
      const overrides = await componentOverrides(path, "PropMesh");
      expect(overrides.BoundsScale).toBeDefined();
      expect(Number.parseFloat(overrides.BoundsScale!.value!)).toBeCloseTo(3.5, 4);
      // The archetype default is 1.0 — proves this is a real template override.
      expect(Number.parseFloat(overrides.BoundsScale!.archetype_value!)).toBeCloseTo(1.0, 4);
    } finally {
      await ensureAbsent(ctx.mcp, path);
    }
  });

  test("set_component_transform_then_read_back", async () => {
    // Set the relative transform on a component template, then read the
    // RelativeLocation/Rotation/Scale3D overrides back through bp_read.
    const path = `${NS}/BP_CompXform`;
    await ensureAbsent(ctx.mcp, path);
    try {
      await ctx.mcp.expect("bp_create_blueprint", { name: path, parent_class: "Actor" });
      await ctx.mcp.expect("bp_add_component", {
        blueprint_name: path,
        component_type: "StaticMeshComponent",
        component_name: "XformMesh",
      });
      await ctx.mcp.expect("bp_set_component_transform", {
        blueprint_name: path,
        component_name: "XformMesh",
        location: [10, 20, 30],
        rotation: [0, 90, 0],
        scale: [2, 2, 2],
      });
      const overrides = await componentOverrides(path, "XformMesh");
      // Values are the engine's on-disk export text, e.g. "(X=10.000000,...)".
      expect(overrides.RelativeLocation?.value).toBe("(X=10.000000,Y=20.000000,Z=30.000000)");
      expect(overrides.RelativeRotation?.value).toContain("Yaw=90.000000");
      expect(overrides.RelativeScale3D?.value).toBe("(X=2.000000,Y=2.000000,Z=2.000000)");
    } finally {
      await ensureAbsent(ctx.mcp, path);
    }
  });

  test("set_class_replication_observed_on_spawned_instance", async () => {
    // Flip bReplicates/bAlwaysRelevant on the Blueprint CDO, then observe the
    // flags on a FRESHLY SPAWNED instance of the generated class via
    // actor_set_property's dry-run diff (its `before` field re-exports the live
    // property by reflection and never mutates) — an independent readback.
    // reflection_class_properties can't resolve BP generated classes (its
    // FindFirstObject uses ExactClass, which excludes UBlueprintGeneratedClass),
    // so the spawned-instance read is the observation path.
    const path = `${NS}/BP_ClassRepl`;
    const actorName = "MCPTest_BP_ClassRepl";
    const deleteActor = async () => {
      try {
        await ctx.mcp.command("actor_delete", { name: actorName });
      } catch {
        /* ignore */
      }
    };
    await ensureAbsent(ctx.mcp, path);
    try {
      await ctx.mcp.expect("bp_create_blueprint", { name: path, parent_class: "Actor" });
      await ctx.mcp.expect("bp_compile", { blueprint_name: path });
      const result = await ctx.mcp.expect("bp_set_class_replication", {
        blueprint_name: path,
        replicates: true,
        always_relevant: true,
      });
      expect(result.success).toBe(true);

      // Observe: a new instance initializes from the mutated CDO.
      await deleteActor();
      await ctx.mcp.expect("actor_spawn", {
        class_path: `${path}.BP_ClassRepl_C`,
        name: actorName,
      });
      for (const prop of ["bReplicates", "bAlwaysRelevant"]) {
        const probe = await ctx.mcp.expect("actor_set_property", {
          name: actorName,
          property: prop,
          value: true,
          dry_run: true,
        });
        const diff = probe.diff as { properties_changed: Array<{ before: string }> };
        expect(diff.properties_changed[0]?.before).toBe("True");
      }
    } finally {
      await deleteActor();
      await ensureAbsent(ctx.mcp, path);
    }
  });

  test("create_variable_dry_run_does_not_mutate", async () => {
    const result = await ctx.mcp.expect("bp_create_variable", {
      blueprint_name: SAMPLE,
      variable_name: "ShouldNotExist",
      variable_type: "bool",
      dry_run: true,
    });
    expect(result.dry_run).toBe(true);
    // The dry-run diff must describe the would-be variable...
    const diff = (result.diff ?? {}) as { variables_added?: Array<{ variable_name?: string }> };
    expect(diff.variables_added?.[0]?.variable_name).toBe("ShouldNotExist");
    // ...and the variable must be ABSENT — proved via an independent read.
    const content = await ctx.mcp.expect("bp_read", { blueprint_path: SAMPLE });
    const names = ((content.variables ?? []) as Array<{ name?: string }>).map((v) => v.name);
    expect(names).not.toContain("ShouldNotExist");
  });
});
