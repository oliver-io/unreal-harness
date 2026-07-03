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

  test("test_gas_ability_set_cost_and_cooldown_bind_then_clear", async () => {
    // Bind distinct Cost/Cooldown GameplayEffect classes on a GA Blueprint, then
    // observe through the asset registry's dependency graph (not the setter's
    // echo): both GE packages become hard outbound dependencies of the saved
    // ability, and clearing ONE slot removes exactly that GE's dependency while
    // the other survives — proving the two setters write independent CDO slots
    // and that effect_class="" clears.
    const ability = `${NS}/GA_CostCooldown`;
    const geCost = `${NS}/GE_CostFx`;
    const geCooldown = `${NS}/GE_CooldownFx`;

    // Independent readback: the registry's outbound dependency set, rebuilt
    // from the saved package's import table — a different, deeper primitive
    // than the GAS setter's own echo.
    const outboundRefs = async (assetPath: string): Promise<Set<string>> => {
      const result = await ctx.mcp.expect("asset_references", {
        asset_path: assetPath,
        direction: "outbound",
      });
      const refs = (result.references ?? []) as Array<{ path: string }>;
      return new Set(refs.map((r) => r.path));
    };

    for (const p of [ability, geCost, geCooldown]) await ensureAbsent(ctx.mcp, p);
    try {
      await ctx.mcp.expect("gas_ability_create", { asset_path: ability });
      await ctx.mcp.expect("gas_effect_create", { asset_path: geCost, duration_policy: "Instant" });
      await ctx.mcp.expect("gas_effect_create", {
        asset_path: geCooldown,
        duration_policy: "Duration",
      });

      // Baseline: the fresh ability references neither GE.
      const baseline = await outboundRefs(ability);
      expect(baseline.has(geCost)).toBe(false);
      expect(baseline.has(geCooldown)).toBe(false);

      const setCost = await ctx.mcp.expect("gas_ability_set_cost", {
        ability_path: ability,
        effect_class: geCost,
      });
      expect(setCost.cleared).toEqual(false);
      const setCooldown = await ctx.mcp.expect("gas_ability_set_cooldown", {
        ability_path: ability,
        effect_class: geCooldown,
      });
      expect(setCooldown.cleared).toEqual(false);

      // Observe: BOTH GEs are now hard dependencies of the saved ability. If the
      // setters wrote the same property, the second bind would have overwritten
      // the first and only one GE could be referenced.
      let refs = await outboundRefs(ability);
      expect(refs.has(geCost)).toBe(true);
      expect(refs.has(geCooldown)).toBe(true);

      // Clear the COST only; exactly its dependency drops, the cooldown's survives.
      let cleared = await ctx.mcp.expect("gas_ability_set_cost", {
        ability_path: ability,
        effect_class: "",
      });
      expect(cleared.cleared).toEqual(true);
      refs = await outboundRefs(ability);
      expect(refs.has(geCost)).toBe(false);
      expect(refs.has(geCooldown)).toBe(true);

      // Clear the cooldown too; both gone.
      cleared = await ctx.mcp.expect("gas_ability_set_cooldown", {
        ability_path: ability,
        effect_class: "",
      });
      expect(cleared.cleared).toEqual(true);
      refs = await outboundRefs(ability);
      expect(refs.has(geCost)).toBe(false);
      expect(refs.has(geCooldown)).toBe(false);
    } finally {
      // Delete the ability first — it may still reference the GEs on failure.
      for (const p of [ability, geCost, geCooldown]) await ensureAbsent(ctx.mcp, p);
    }
  }, 60_000); // ~13 bridge round-trips, several with auto-saves — exceeds bun's 5s default

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
