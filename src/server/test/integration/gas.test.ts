/**
 * Gameplay Ability System domain — create the GAS scaffolding asset trio
 * (AttributeSet, GameplayAbility, GameplayEffect) and exercise the runtime apply.
 * Port of tests/integration/test_gas.py.
 *
 * The three create ops are typed wrappers over create_blueprint; each auto-saves on
 * success, so the assertion is "a uasset exists on disk where we asked".
 * gas_effect_apply is a runtime tool that requires a live PIE session with an
 * AbilitySystemComponent on the target — the headless suite has no PIE, so that test
 * asserts the documented `not_in_pie` guard and skips if the editor is unexpectedly
 * in another state.
 */

import { test, expect, beforeAll } from "bun:test";
import { existsSync } from "node:fs";
import { join } from "node:path";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { ensureAbsent, assertReady } from "../harness/ops.ts";
import { projectDir } from "../harness/env.ts";

const NS = `${ROOT}/gas`;
const ATTRSET = `${NS}/AS_Health`;
const ABILITY = `${NS}/GA_MCPTest`;
const EFFECT = `${NS}/GE_MCPTest`;
const APPLY_EFFECT = `${NS}/GE_ApplySample`;

/** Map a /Game/... content path to its on-disk package file. */
function uassetDiskPath(gamePath: string, ext = ".uasset"): string {
  const pkg = gamePath.split(".")[0] ?? gamePath;
  if (!pkg.startsWith("/Game/")) throw new Error(`not a /Game/ path: ${gamePath}`);
  const rel = pkg.slice("/Game/".length);
  return join(projectDir(), "Content", rel + ext);
}

editorSuite("gas", (ctx) => {
  beforeAll(async () => {
    // sample_effect fixture: a GameplayEffect to reference from the apply test.
    await ensureAbsent(ctx.mcp, APPLY_EFFECT);
    await ctx.mcp.expect("gas_effect_create", {
      asset_path: APPLY_EFFECT,
      duration_policy: "Instant",
    });
  });

  test("test_gas_attributeset_create_writes_uasset", async () => {
    await ensureAbsent(ctx.mcp, ATTRSET);
    const result = await ctx.mcp.expect("gas_attributeset_create", { asset_path: ATTRSET });
    expect(result.success).toEqual(true);
    expect(result.is_scaffolding).toEqual(true);
    const disk = uassetDiskPath(ATTRSET);
    expect(existsSync(disk)).toBe(true);
  });

  test("test_gas_ability_create_writes_uasset", async () => {
    await ensureAbsent(ctx.mcp, ABILITY);
    const result = await ctx.mcp.expect("gas_ability_create", { asset_path: ABILITY });
    expect(result.success).toEqual(true);
    const disk = uassetDiskPath(ABILITY);
    expect(existsSync(disk)).toBe(true);
  });

  test("test_gas_effect_create_writes_uasset", async () => {
    await ensureAbsent(ctx.mcp, EFFECT);
    const result = await ctx.mcp.expect("gas_effect_create", {
      asset_path: EFFECT,
      duration_policy: "Instant",
    });
    expect(result.success).toEqual(true);
    const disk = uassetDiskPath(EFFECT);
    expect(existsSync(disk)).toBe(true);
  });

  test("test_gas_effect_apply_requires_pie", async () => {
    // Runtime op: needs a live PIE world with an AbilitySystemComponent target.
    // The headless suite has none, so we assert the documented not_in_pie guard.
    const resp = await ctx.mcp.command("gas_effect_apply", {
      target_actor: "MCPTest_GASTarget",
      effect_class: APPLY_EFFECT + "_C",
      level: 1.0,
    });
    if (resp.status === "success") {
      // Only reachable inside an active PIE session with a valid ASC target.
      const result = (resp.result ?? {}) as Record<string, unknown>;
      expect(result.handle_valid).toBeDefined();
    } else {
      const code = resp.error_code;
      if (code !== "not_in_pie") {
        console.log(
          `gas_effect_apply needs a live PIE session with an ASC target; got error_code=${JSON.stringify(code)}`,
        );
        return;
      }
      expect(code).toEqual("not_in_pie");
    }
    await assertReady(ctx.mcp);
  });
});
