#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * GAS authoring tools — UGameplayAbility / UGameplayEffect / UAttributeSet
 * Blueprint scaffolding plus runtime effect application.
 * Mirrors mcp/docs/todo/10_gas_authoring.md.
 *
 * Surface (rolling):
 *   - gas_ability_create          (now validates + applies `tags` → AbilityTags)
 *   - gas_ability_set_cost        (assign the cost GameplayEffect class)
 *   - gas_ability_set_cooldown    (assign the cooldown GameplayEffect class)
 *   - gas_effect_create
 *   - gas_effect_apply (PIE only)
 *   - gas_attributeset_create
 *   - gas_asc_add_ability         (deferred)
 *
 * Hard prerequisite (doc 9 — Gameplay Tag Registry) is satisfied: tag inputs
 * validate against the registered tag set and fail closed with `unknown_tag`.
 * Note: on UGameplayAbility, "cost" and "cooldown" are GameplayEffect *classes*,
 * not tags — hence the dedicated set_cost / set_cooldown tools rather than tag
 * params on create.
 */
class FMCPGASCommands
{
public:
    FMCPGASCommands() = default;

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleAbilityCreate(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAbilitySetCost(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAbilitySetCooldown(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleEffectCreate(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleEffectApply(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAttributeSetCreate(const TSharedPtr<FJsonObject>& Params);

    // Shared implementation for set_cost / set_cooldown: assign a GameplayEffect
    // class to the named protected TSubclassOf property on the ability CDO (via
    // reflection, since the property is protected). `effect_class` empty clears it.
    TSharedPtr<FJsonObject> SetAbilityGameplayEffectClass(
        const TSharedPtr<FJsonObject>& Params, const TCHAR* PropertyName, const TCHAR* WhichLabel);
};
