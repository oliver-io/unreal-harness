/**
 * Domain: gas — Gameplay Ability System asset authoring + runtime effect apply.
 *
 * Port of the `gas_*` tools in `src/MCP/server.py`. Three typed asset-creation
 * wrappers (ability / effect / attributeset Blueprints) plus one runtime mutator
 * that applies a GameplayEffect to a live PIE actor.
 *
 * Wire commands equal the tool names. All four are in `IsBlockedDuringPie`; the
 * three create tools are in `IsBlockedFromDryRun` (gas_effect_apply is a runtime
 * tool — dry-run deferred, so it is intentionally absent from that list).
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const gasAbilityCreate = bridgeTool({
  name: "gas_ability_create",
  domain: "gas",
  description:
    "Create a UGameplayAbility Blueprint asset. Typed wrapper over create_blueprint " +
    "(parent defaults to UGameplayAbility). Give (path, name) or asset_path. " +
    "Auto-saves; path uniqueness enforced. Optional tags[] are set as the ability's " +
    "AbilityTags/AssetTags — each must already be registered (tag_add) or the call " +
    "fails closed with unknown_tag (no orphan asset is left behind). NOTE: cost and " +
    "cooldown are NOT tags on a GameplayAbility — they are GameplayEffect classes; set " +
    "them with gas_ability_set_cost / gas_ability_set_cooldown.",
  input: z.object({
    path: z
      .string()
      .default("")
      .describe('Destination package path ("/Game/GAS/Abilities").'),
    name: z.string().default("").describe('Asset short name ("GA_Fireball").'),
    asset_path: z
      .string()
      .default("")
      .describe("Convenience combined path; replaces (path, name)."),
    parent_class: z
      .string()
      .default("")
      .describe(
        "Optional UGameplayAbility subclass — script path, asset path, or short name. " +
          "Defaults to UGameplayAbility itself.",
      ),
    tags: z
      .array(z.string())
      .default([])
      .describe(
        "Optional gameplay tags to set as the ability's AbilityTags/AssetTags. Each must " +
          "already be registered (tag_add); an unregistered tag fails the whole call with " +
          "unknown_tag. These tags are behavioral (drive cancel/block/activation matching), " +
          "not cosmetic labels.",
      ),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.path) p.path = a.path;
    if (a.name) p.name = a.name;
    if (a.asset_path) p.asset_path = a.asset_path;
    if (a.parent_class) p.parent_class = a.parent_class;
    if (a.tags.length) p.tags = a.tags;
    return p;
  },
});

const gasAbilitySetCost = bridgeTool({
  name: "gas_ability_set_cost",
  domain: "gas",
  description:
    "Assign the COST GameplayEffect class on a UGameplayAbility Blueprint (the engine's " +
    "actual cost model — committing the ability applies this GE to pay mana/stamina/etc). " +
    "ability_path = the GA Blueprint; effect_class = a UGameplayEffect (sub)class path. " +
    'Pass effect_class="" to CLEAR the cost. Auto-saves the ability.',
  input: z.object({
    ability_path: z
      .string()
      .min(1)
      .describe("UGameplayAbility Blueprint path (e.g. /Game/GAS/Abilities/GA_Dash)."),
    effect_class: z
      .string()
      .default("")
      .describe(
        "UGameplayEffect (sub)class — GE Blueprint path (_C appended automatically), native " +
          'class, or short name. Empty string ("") clears the cost binding.',
      ),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = { ability_path: a.ability_path };
    p.effect_class = a.effect_class; // empty string is meaningful (clear)
    return p;
  },
});

const gasAbilitySetCooldown = bridgeTool({
  name: "gas_ability_set_cooldown",
  domain: "gas",
  description:
    "Assign the COOLDOWN GameplayEffect class on a UGameplayAbility Blueprint (the engine's " +
    "actual cooldown model — committing the ability applies this GE, and the ability cannot " +
    "be used again until it expires; the cooldown's duration + tags live on that GE). " +
    "ability_path = the GA Blueprint; effect_class = a UGameplayEffect (sub)class path. " +
    'Pass effect_class="" to CLEAR the cooldown. Auto-saves the ability.',
  input: z.object({
    ability_path: z
      .string()
      .min(1)
      .describe("UGameplayAbility Blueprint path (e.g. /Game/GAS/Abilities/GA_Dash)."),
    effect_class: z
      .string()
      .default("")
      .describe(
        "UGameplayEffect (sub)class — GE Blueprint path (_C appended automatically), native " +
          'class, or short name. Empty string ("") clears the cooldown binding.',
      ),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = { ability_path: a.ability_path };
    p.effect_class = a.effect_class; // empty string is meaningful (clear)
    return p;
  },
});

const gasEffectCreate = bridgeTool({
  name: "gas_effect_create",
  domain: "gas",
  description:
    "Create a UGameplayEffect Blueprint asset. Give (path, name) or asset_path. " +
    "parent_class defaults to UGameplayEffect. duration_policy is a closed set: " +
    '"Instant" | "Duration" | "Infinite". Auto-saves; path uniqueness enforced.',
  input: z.object({
    path: z
      .string()
      .default("")
      .describe('Destination package path ("/Game/GAS/Effects").'),
    name: z.string().default("").describe('Asset short name ("GE_FireDoT").'),
    asset_path: z.string().default("").describe("Convenience combined path."),
    parent_class: z
      .string()
      .default("")
      .describe(
        "Optional UGameplayEffect subclass — script path, asset path, or short name. " +
          "Defaults to UGameplayEffect itself.",
      ),
    duration_policy: z
      .string()
      .default("")
      .describe(
        'Optional. Closed set: "Instant" | "Duration" (alias for HasDuration) | ' +
          '"Infinite". Sets DurationPolicy on the generated CDO when provided.',
      ),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.path) p.path = a.path;
    if (a.name) p.name = a.name;
    if (a.asset_path) p.asset_path = a.asset_path;
    if (a.parent_class) p.parent_class = a.parent_class;
    if (a.duration_policy) p.duration_policy = a.duration_policy;
    return p;
  },
});

const gasEffectApply = bridgeTool({
  name: "gas_effect_apply",
  domain: "gas",
  description:
    "Apply a UGameplayEffect to a target actor in PIE. Runtime tool — requires an " +
    "active PIE session (returns not_in_pie otherwise). target_actor must own a " +
    "UAbilitySystemComponent. Returns handle_valid.",
  input: z.object({
    target_actor: z
      .string()
      .min(1)
      .describe(
        "FName of the actor in the live PIE world. Must own (or implement " +
          "IAbilitySystemInterface returning) a UAbilitySystemComponent.",
      ),
    effect_class: z
      .string()
      .min(1)
      .describe(
        "UGameplayEffect (sub)class — script path, asset path (_C appended " +
          "automatically), or short name.",
      ),
    level: z
      .number()
      .default(1.0)
      .describe("Effect magnitude scalar. Default 1.0."),
    instigator: z
      .string()
      .default("")
      .describe(
        'Optional FName of the actor "responsible" for the effect (damage ' +
          "attribution, FX context). Omitted → self-instigated context.",
      ),
  }),
  annotations: { blockedDuringPie: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      target_actor: a.target_actor,
      effect_class: a.effect_class,
      level: a.level,
    };
    if (a.instigator) p.instigator = a.instigator;
    return p;
  },
});

const gasAttributesetCreate = bridgeTool({
  name: "gas_attributeset_create",
  domain: "gas",
  description:
    "Create a Blueprintable UAttributeSet asset (scaffolding tier — prototyping " +
    "path; production AttributeSets are typically C++). Give (path, name) or " +
    "asset_path. parent_class defaults to UAttributeSet. Response includes " +
    "is_scaffolding=true. Seed attributes via bp_create_variable.",
  input: z.object({
    path: z
      .string()
      .default("")
      .describe('Destination package path ("/Game/GAS/AttributeSets").'),
    name: z.string().default("").describe('Asset short name ("AS_Health").'),
    asset_path: z.string().default("").describe("Convenience combined path."),
    parent_class: z
      .string()
      .default("")
      .describe(
        "Optional UAttributeSet subclass — script path, asset path, or short name. " +
          "Defaults to UAttributeSet itself.",
      ),
  }),
  annotations: { blockedDuringPie: true, dryRunUnsupported: true },
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.path) p.path = a.path;
    if (a.name) p.name = a.name;
    if (a.asset_path) p.asset_path = a.asset_path;
    if (a.parent_class) p.parent_class = a.parent_class;
    return p;
  },
});

export const gasTools: ToolDef[] = [
  gasAbilityCreate,
  gasAbilitySetCost,
  gasAbilitySetCooldown,
  gasEffectCreate,
  gasEffectApply,
  gasAttributesetCreate,
];
