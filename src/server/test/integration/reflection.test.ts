/**
 * Reflection domain — pure read-only introspection of the editor's live UClass
 * reflection database. No assets are created or mutated. Port of
 * tests/integration/test_reflection.py.
 *
 * These exercise the three class-introspection ops against well-known native engine
 * classes (Actor / PointLight), which are always loaded, so the assertions hold in an
 * empty project.
 */

import { test, expect } from "bun:test";
import { editorSuite } from "../harness/suite.ts";

editorSuite("reflection", (ctx) => {
  test("class_query_finds_pointlight", async () => {
    const result = await ctx.mcp.expect("class_query", { name_pattern: "PointLight" });
    const classes: any[] = (result.classes as any[]) ?? [];
    expect(Array.isArray(classes) && classes.length > 0).toBeTruthy();
    const names = classes.map((c: any) => c.name);
    expect(names.some((n: any) => (n || "").includes("PointLight"))).toBeTruthy();
    // Each entry carries the documented shape.
    expect(classes.every((c: any) => "path" in c && "name" in c)).toBeTruthy();
  });

  test("class_inspect_actor", async () => {
    const result = await ctx.mcp.expect("class_inspect", {
      class_name: "Actor",
      include: ["properties", "hierarchy"],
    });
    expect(String(result.class_path ?? "").toLowerCase()).toContain("actor");
    const props: any[] = (result.properties as any[]) ?? [];
    expect(Array.isArray(props) && props.length > 0).toBeTruthy();
    expect(props.every((p: any) => "name" in p)).toBeTruthy();
    // hierarchy is the parent chain inclusive of the class itself.
    expect(result.hierarchy).toBeTruthy();
  });

  test("get_class_properties_actor", async () => {
    const result = await ctx.mcp.expect("reflection_class_properties", {
      class_name: "Actor",
      include_inherited: false,
    });
    expect(
      String(result.class_name ?? "").toLowerCase().includes("actor") ||
        String(result.class_path ?? "").toLowerCase().includes("actor"),
    ).toBeTruthy();
    const props: any[] = (result.properties as any[]) ?? [];
    expect(Array.isArray(props) && props.length > 0).toBeTruthy();
    expect(props.every((p: any) => "name" in p && "type" in p)).toBeTruthy();
  });
});
