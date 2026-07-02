#include "Commands/MCPGASCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Factories/BlueprintFactory.h"
#include "Kismet2/BlueprintEditorUtils.h"

#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "GameplayTagsManager.h"
#include "GameplayTagContainer.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"

namespace
{
    bool ResolveGASDestination(const TSharedPtr<FJsonObject>& Params, FString& OutPackagePath, FString& OutAssetName, FString& OutFullPath, FString& OutError)
    {
        FString Path, Name;
        Params->TryGetStringField(TEXT("path"), Path);
        Params->TryGetStringField(TEXT("name"), Name);

        if (Path.IsEmpty() || Name.IsEmpty())
        {
            FString Combined;
            if (Params->TryGetStringField(TEXT("asset_path"), Combined) && !Combined.IsEmpty())
            {
                int32 LastSlash = INDEX_NONE;
                if (Combined.FindLastChar(TEXT('/'), LastSlash))
                {
                    Path = Combined.Left(LastSlash);
                    Name = Combined.Mid(LastSlash + 1);
                }
            }
        }
        if (Path.IsEmpty() || Name.IsEmpty())
        {
            OutError = TEXT("Missing destination — pass either {path, name} or {asset_path}.");
            return false;
        }
        if (!Path.StartsWith(TEXT("/")))
        {
            OutError = FString::Printf(TEXT("Path must begin with '/Game/' or '/<Plugin>/' — got: %s"), *Path);
            return false;
        }
        OutPackagePath = Path;
        OutAssetName = Name;
        OutFullPath = Path / Name;
        return true;
    }

    UClass* ResolveBPParent(const FString& Name, UClass* DefaultClass)
    {
        if (Name.IsEmpty())
        {
            return DefaultClass;
        }
        if (Name.StartsWith(TEXT("/")))
        {
            FString Path = Name;
            if (!Path.Contains(TEXT(".")))
            {
                const FString ShortName = FPackageName::GetShortName(Path);
                Path = FString::Printf(TEXT("%s.%s_C"), *Path, *ShortName);
            }
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *Path))
            {
                return Loaded;
            }
        }
        if (UClass* Found = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::ExactClass))
        {
            return Found;
        }
        return nullptr;
    }

    UBlueprint* CreateBlueprintViaFactory(UClass* ParentClass, const FString& AssetName, const FString& PackagePath)
    {
        UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
        Factory->ParentClass = ParentClass;

        FAssetToolsModule& AssetToolsModule =
            FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
        UObject* Created = AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, UBlueprint::StaticClass(), Factory);
        return Cast<UBlueprint>(Created);
    }

    bool TryParseDurationPolicy(const FString& In, EGameplayEffectDurationType& Out)
    {
        const FString Lower = In.ToLower();
        if (Lower == TEXT("instant"))                 { Out = EGameplayEffectDurationType::Instant;     return true; }
        if (Lower == TEXT("infinite"))                { Out = EGameplayEffectDurationType::Infinite;    return true; }
        // The doc's "Duration" is the wire name; UE's enum spells it HasDuration. Accept both.
        if (Lower == TEXT("duration"))                { Out = EGameplayEffectDurationType::HasDuration; return true; }
        if (Lower == TEXT("hasduration"))             { Out = EGameplayEffectDurationType::HasDuration; return true; }
        if (Lower == TEXT("has_duration"))            { Out = EGameplayEffectDurationType::HasDuration; return true; }
        return false;
    }

    // Validate each string against the gameplay-tag registry WITHOUT registering it
    // (RequestGameplayTag with ErrorIfNotFound=false just looks up the dictionary).
    // On the first unregistered tag, returns false and sets OutBadTag. Accumulates the
    // valid tags into OutContainer. This is how gas_ability_create fails closed with
    // `unknown_tag` rather than silently coining new tags.
    bool ValidateRegisteredTags(const TArray<FString>& TagStrings, FGameplayTagContainer& OutContainer, FString& OutBadTag)
    {
        UGameplayTagsManager& Mgr = UGameplayTagsManager::Get();
        for (const FString& S : TagStrings)
        {
            if (S.IsEmpty()) continue;
            const FGameplayTag Tag = Mgr.RequestGameplayTag(FName(*S), /*ErrorIfNotFound=*/false);
            if (!Tag.IsValid())
            {
                OutBadTag = S;
                return false;
            }
            OutContainer.AddTag(Tag);
        }
        return true;
    }
}

TSharedPtr<FJsonObject> FMCPGASCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("gas_ability_create"))        return HandleAbilityCreate(Params);
    if (CommandType == TEXT("gas_ability_set_cost"))      return HandleAbilitySetCost(Params);
    if (CommandType == TEXT("gas_ability_set_cooldown"))  return HandleAbilitySetCooldown(Params);
    if (CommandType == TEXT("gas_effect_create"))         return HandleEffectCreate(Params);
    if (CommandType == TEXT("gas_effect_apply"))          return HandleEffectApply(Params);
    if (CommandType == TEXT("gas_attributeset_create"))   return HandleAttributeSetCreate(Params);

    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown GAS command: %s"), *CommandType),
        EMCPErrorCode::InvalidArgument,
        TEXT("Supported GAS commands in this build: gas_ability_create, gas_ability_set_cost, gas_ability_set_cooldown, gas_effect_create, gas_effect_apply, gas_attributeset_create."));
}

TSharedPtr<FJsonObject> FMCPGASCommands::HandleAbilityCreate(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, AssetName, FullAssetPath, ResolveError;
    if (!ResolveGASDestination(Params, PackagePath, AssetName, FullAssetPath, ResolveError))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            ResolveError,
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass {path:'/Game/GAS/Abilities', name:'GA_Foo'} or {asset_path:'/Game/GAS/Abilities/GA_Foo'}."));
    }
    if (UEditorAssetLibrary::DoesAssetExist(FullAssetPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s"), *FullAssetPath),
            EMCPErrorCode::NameCollision,
            TEXT("No silent overwrite. Pick a different path or delete the existing asset first."));
    }

    FString ParentClassPath;
    Params->TryGetStringField(TEXT("parent_class"), ParentClassPath);
    UClass* ParentClass = ResolveBPParent(ParentClassPath, UGameplayAbility::StaticClass());
    if (!ParentClass)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve parent class: %s"), *ParentClassPath),
            EMCPErrorCode::ClassNotLoaded,
            TEXT("Pass a UGameplayAbility subclass — script path, asset path, or short name. Defaults to UGameplayAbility itself."));
    }
    if (!ParentClass->IsChildOf(UGameplayAbility::StaticClass()))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Parent class is not a UGameplayAbility subclass: %s"), *ParentClass->GetPathName()),
            EMCPErrorCode::UnsupportedClass,
            TEXT("gas_ability_create only creates UGameplayAbility Blueprints. Use create_blueprint for arbitrary parents."));
    }

    // Optional `tags` → AbilityTags/AssetTags. Validate against the registry FIRST,
    // before creating the asset, so an unregistered tag fails closed with `unknown_tag`
    // and never leaves an orphan Blueprint on disk. (cost/cooldown are GameplayEffect
    // classes on UGameplayAbility, not tags — see gas_ability_set_cost/set_cooldown.)
    FGameplayTagContainer AbilityTagsToApply;
    {
        const TArray<TSharedPtr<FJsonValue>>* TagsArr = nullptr;
        if (Params->TryGetArrayField(TEXT("tags"), TagsArr) && TagsArr)
        {
            TArray<FString> TagStrings;
            for (const TSharedPtr<FJsonValue>& V : *TagsArr)
            {
                FString S;
                if (V.IsValid() && V->TryGetString(S)) TagStrings.Add(S);
            }
            FString BadTag;
            if (!ValidateRegisteredTags(TagStrings, AbilityTagsToApply, BadTag))
            {
                return FMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Unknown gameplay tag: %s"), *BadTag),
                    EMCPErrorCode::UnknownTag,
                    TEXT("The tag is not in the registry. Register it first with tag_add (or check tag_list), then retry. Tags are not auto-created."));
            }
        }
    }

    UBlueprint* Created = CreateBlueprintViaFactory(ParentClass, AssetName, PackagePath);
    if (!Created)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("UAssetTools::CreateAsset returned null for the GameplayAbility Blueprint"),
            EMCPErrorCode::EngineBusy,
            TEXT("UE rejected the creation; check the editor log."));
    }

    // Apply the validated tags to the freshly-generated class's CDO. AbilityTags is
    // mutated through the editor-only accessor EditorGetAssetTags() (the direct member
    // is deprecated-for-game). BP CDOs hang off GeneratedClass.
    TArray<TSharedPtr<FJsonValue>> AppliedTags;
#if WITH_EDITOR
    if (!AbilityTagsToApply.IsEmpty())
    {
        if (UClass* GenClass = Created->GeneratedClass)
        {
            if (UGameplayAbility* CDO = Cast<UGameplayAbility>(GenClass->GetDefaultObject()))
            {
                CDO->EditorGetAssetTags().AppendTags(AbilityTagsToApply);
                CDO->MarkPackageDirty();
                for (const FGameplayTag& T : AbilityTagsToApply)
                {
                    AppliedTags.Add(MakeShared<FJsonValueString>(T.ToString()));
                }
            }
        }
    }
#endif

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Created);
    Created->MarkPackageDirty();
    const bool bSaved = UEditorAssetLibrary::SaveAsset(FullAssetPath, /*bOnlyIfIsDirty=*/false);
    if (!bSaved)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("GameplayAbility Blueprint created in memory but not saved to disk: %s"), *FullAssetPath),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — the save silently no-ops while PIE is active or under an unattended/headless editor, leaving the asset dirty but unwritten. Stop PIE and retry, or save the asset manually."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("path"), FullAssetPath);
    ResultObj->SetStringField(TEXT("class"), TEXT("/Script/Engine.Blueprint"));
    ResultObj->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
    if (AppliedTags.Num() > 0)
    {
        ResultObj->SetArrayField(TEXT("ability_tags"), AppliedTags);
    }
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ─── gas_ability_set_cost / gas_ability_set_cooldown ────────────────────────

TSharedPtr<FJsonObject> FMCPGASCommands::HandleAbilitySetCost(const TSharedPtr<FJsonObject>& Params)
{
    return SetAbilityGameplayEffectClass(Params, TEXT("CostGameplayEffectClass"), TEXT("cost"));
}

TSharedPtr<FJsonObject> FMCPGASCommands::HandleAbilitySetCooldown(const TSharedPtr<FJsonObject>& Params)
{
    return SetAbilityGameplayEffectClass(Params, TEXT("CooldownGameplayEffectClass"), TEXT("cooldown"));
}

TSharedPtr<FJsonObject> FMCPGASCommands::SetAbilityGameplayEffectClass(
    const TSharedPtr<FJsonObject>& Params, const TCHAR* PropertyName, const TCHAR* WhichLabel)
{
    // Resolve the target ability Blueprint.
    FString AbilityPath;
    if (!Params->TryGetStringField(TEXT("ability_path"), AbilityPath) || AbilityPath.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'ability_path'"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`ability_path` is required — the UGameplayAbility Blueprint to modify (e.g. /Game/GAS/Abilities/GA_Dash)."));
    }

    UObject* Loaded = UEditorAssetLibrary::LoadAsset(AbilityPath);
    UBlueprint* AbilityBP = Cast<UBlueprint>(Loaded);
    if (!AbilityBP || !AbilityBP->GeneratedClass ||
        !AbilityBP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Not a GameplayAbility Blueprint: %s"), *AbilityPath),
            EMCPErrorCode::UnsupportedClass,
            TEXT("`ability_path` must resolve to a UGameplayAbility Blueprint (created via gas_ability_create or with a UGameplayAbility parent)."));
    }

    UGameplayAbility* CDO = Cast<UGameplayAbility>(AbilityBP->GeneratedClass->GetDefaultObject());
    if (!CDO)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Could not access the ability's class-default object"),
            EMCPErrorCode::Internal,
            TEXT("The Blueprint's GeneratedClass has no UGameplayAbility CDO; try recompiling the Blueprint (bp_compile)."));
    }

    // Resolve the GameplayEffect class. Empty `effect_class` CLEARS the binding.
    FString EffectClassPath;
    Params->TryGetStringField(TEXT("effect_class"), EffectClassPath);
    UClass* EffectClass = nullptr; // nullptr == clear
    if (!EffectClassPath.IsEmpty())
    {
        EffectClass = ResolveBPParent(EffectClassPath, nullptr);
        if (!EffectClass)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Could not resolve GameplayEffect class: %s"), *EffectClassPath),
                EMCPErrorCode::ClassNotLoaded,
                TEXT("`effect_class` must be a UGameplayEffect subclass — a GE Blueprint path (e.g. /Game/GAS/Effects/GE_DashCost) or native class. Create one with gas_effect_create."));
        }
        if (!EffectClass->IsChildOf(UGameplayEffect::StaticClass()))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Class is not a UGameplayEffect subclass: %s"), *EffectClass->GetPathName()),
                EMCPErrorCode::UnsupportedClass,
                FString::Printf(TEXT("The %s of an ability must be a UGameplayEffect class."), WhichLabel));
        }
    }

    // Write the protected TSubclassOf property by reflection (FClassProperty derives
    // from FObjectPropertyBase; the stored value is the UClass itself).
    FProperty* Prop = CDO->GetClass()->FindPropertyByName(FName(PropertyName));
    FObjectPropertyBase* ClassProp = CastField<FObjectPropertyBase>(Prop);
    if (!ClassProp)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Property '%s' not found on the ability class"), PropertyName),
            EMCPErrorCode::Internal,
            TEXT("The engine's UGameplayAbility cost/cooldown property could not be located by reflection — engine version mismatch."));
    }
    ClassProp->SetObjectPropertyValue_InContainer(CDO, EffectClass);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AbilityBP);
    AbilityBP->MarkPackageDirty();
    CDO->MarkPackageDirty();
    const bool bSaved = UEditorAssetLibrary::SaveAsset(AbilityBP->GetPathName(), /*bOnlyIfIsDirty=*/false);
    if (!bSaved)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Set %s but failed to save: %s"), WhichLabel, *AbilityPath),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — the save silently no-ops while PIE is active or under an unattended/headless editor. Stop PIE and retry."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("ability_path"), AbilityBP->GetPathName());
    ResultObj->SetStringField(TEXT("which"), WhichLabel);
    if (EffectClass)
    {
        ResultObj->SetStringField(TEXT("effect_class"), EffectClass->GetPathName());
        ResultObj->SetBoolField(TEXT("cleared"), false);
    }
    else
    {
        ResultObj->SetBoolField(TEXT("cleared"), true);
    }
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPGASCommands::HandleEffectCreate(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, AssetName, FullAssetPath, ResolveError;
    if (!ResolveGASDestination(Params, PackagePath, AssetName, FullAssetPath, ResolveError))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            ResolveError,
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass {path:'/Game/GAS/Effects', name:'GE_Foo'} or {asset_path:'/Game/GAS/Effects/GE_Foo'}."));
    }
    if (UEditorAssetLibrary::DoesAssetExist(FullAssetPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s"), *FullAssetPath),
            EMCPErrorCode::NameCollision,
            TEXT("No silent overwrite. Pick a different path or delete the existing asset first."));
    }

    FString ParentClassPath;
    Params->TryGetStringField(TEXT("parent_class"), ParentClassPath);
    UClass* ParentClass = ResolveBPParent(ParentClassPath, UGameplayEffect::StaticClass());
    if (!ParentClass)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve parent class: %s"), *ParentClassPath),
            EMCPErrorCode::ClassNotLoaded,
            TEXT("Pass a UGameplayEffect subclass — script path, asset path, or short name. Defaults to UGameplayEffect itself."));
    }
    if (!ParentClass->IsChildOf(UGameplayEffect::StaticClass()))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Parent class is not a UGameplayEffect subclass: %s"), *ParentClass->GetPathName()),
            EMCPErrorCode::UnsupportedClass,
            TEXT("gas_effect_create only creates UGameplayEffect Blueprints."));
    }

    // Optional duration policy.
    FString DurationPolicyStr;
    EGameplayEffectDurationType DurationPolicy = EGameplayEffectDurationType::Instant;
    bool bHasDurationPolicy = false;
    if (Params->TryGetStringField(TEXT("duration_policy"), DurationPolicyStr) && !DurationPolicyStr.IsEmpty())
    {
        if (!TryParseDurationPolicy(DurationPolicyStr, DurationPolicy))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unknown duration_policy: %s"), *DurationPolicyStr),
                EMCPErrorCode::InvalidArgument,
                TEXT("Closed set: 'Instant', 'Duration' (alias for HasDuration), 'Infinite'."));
        }
        bHasDurationPolicy = true;
    }

    UBlueprint* Created = CreateBlueprintViaFactory(ParentClass, AssetName, PackagePath);
    if (!Created)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("UAssetTools::CreateAsset returned null for the GameplayEffect Blueprint"),
            EMCPErrorCode::EngineBusy,
            TEXT("UE rejected the creation; check the editor log."));
    }

    if (bHasDurationPolicy)
    {
        // Set DurationPolicy on the CDO of the freshly-generated class. BP CDOs are owned by
        // the GeneratedClass; UBlueprint::GeneratedClass is populated by CreateAsset's
        // factory pass.
        if (UClass* GenClass = Created->GeneratedClass)
        {
            if (UGameplayEffect* CDO = Cast<UGameplayEffect>(GenClass->GetDefaultObject()))
            {
                CDO->DurationPolicy = DurationPolicy;
                CDO->MarkPackageDirty();
            }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Created);
    Created->MarkPackageDirty();
    const bool bSaved = UEditorAssetLibrary::SaveAsset(FullAssetPath, /*bOnlyIfIsDirty=*/false);
    if (!bSaved)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("GameplayEffect Blueprint created in memory but not saved to disk: %s"), *FullAssetPath),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — the save silently no-ops while PIE is active or under an unattended/headless editor, leaving the asset dirty but unwritten. Stop PIE and retry, or save the asset manually."));
    }

    const TCHAR* DurationWire =
          DurationPolicy == EGameplayEffectDurationType::Instant     ? TEXT("Instant")
        : DurationPolicy == EGameplayEffectDurationType::Infinite    ? TEXT("Infinite")
        :                                                              TEXT("HasDuration");

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("path"), FullAssetPath);
    ResultObj->SetStringField(TEXT("class"), TEXT("/Script/Engine.Blueprint"));
    ResultObj->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
    if (bHasDurationPolicy)
    {
        ResultObj->SetStringField(TEXT("duration_policy"), DurationWire);
    }
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

namespace
{
    // Find an actor by name in the given world. Used by gas_effect_apply to
    // resolve the target actor in the live PIE world.
    AActor* FindActorByNameInWorld(UWorld* World, const FString& ActorName)
    {
        if (!World || ActorName.IsEmpty())
        {
            return nullptr;
        }
        TArray<AActor*> AllActors;
        UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);
        for (AActor* Actor : AllActors)
        {
            if (Actor && Actor->GetName() == ActorName)
            {
                return Actor;
            }
        }
        return nullptr;
    }
}

TSharedPtr<FJsonObject> FMCPGASCommands::HandleEffectApply(const TSharedPtr<FJsonObject>& Params)
{
    // Doc 10 invariant: runtime tools require PIE. Refuse with not_in_pie if
    // the editor is not running PIE — exercises doc 1's capability.* group.
    UWorld* PIEWorld = GEditor ? GEditor->PlayWorld : nullptr;
    if (!PIEWorld)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("gas_effect_apply requires an active PIE session"),
            EMCPErrorCode::NotInPie,
            TEXT("Start PIE (start_pie / pie_start) before calling runtime GAS tools."));
    }

    FString TargetActorName;
    if (!Params->TryGetStringField(TEXT("target_actor"), TargetActorName) || TargetActorName.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'target_actor' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the actor's FName (not the editor label). find_actors_by_name / actor_query surfaces them."));
    }

    AActor* Target = FindActorByNameInWorld(PIEWorld, TargetActorName);
    if (!Target)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Target actor not found in PIE world: %s"), *TargetActorName),
            EMCPErrorCode::ActorNotFound,
            TEXT("Verify the actor name. PIE worlds rename actors with a suffix like _C_0; check the live name."));
    }

    UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target);
    if (!TargetASC)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Target actor has no UAbilitySystemComponent: %s"), *TargetActorName),
            EMCPErrorCode::UnsupportedClass,
            TEXT("The actor must own (or implement IAbilitySystemInterface returning) a UAbilitySystemComponent. Check that the GAS pawn / character class is correct."));
    }

    FString EffectClassPath;
    if (!Params->TryGetStringField(TEXT("effect_class"), EffectClassPath) || EffectClassPath.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'effect_class' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass a UGameplayEffect (sub)class path — script path or asset path of a GameplayEffect Blueprint."));
    }

    UClass* EffectClass = ResolveBPParent(EffectClassPath, nullptr);
    if (!EffectClass)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve effect_class: %s"), *EffectClassPath),
            EMCPErrorCode::ClassNotLoaded,
            TEXT("Pass an asset path (\"/Game/.../GE_Foo\" — _C is appended), script path, or short name."));
    }
    if (!EffectClass->IsChildOf(UGameplayEffect::StaticClass()))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class is not a UGameplayEffect subclass: %s"), *EffectClass->GetPathName()),
            EMCPErrorCode::UnsupportedClass,
            TEXT("Only UGameplayEffect subclasses (and their Blueprint derivations) can be applied via this tool."));
    }

    double Level = 1.0;
    Params->TryGetNumberField(TEXT("level"), Level);

    // Optional instigator — actor that's "responsible" for the effect (for
    // damage attribution, FX spawning, etc.). If omitted, we leave the context
    // un-instigated (Spec.Context defaults to a blank context handle).
    FString InstigatorName;
    AActor* Instigator = nullptr;
    if (Params->TryGetStringField(TEXT("instigator"), InstigatorName) && !InstigatorName.IsEmpty())
    {
        Instigator = FindActorByNameInWorld(PIEWorld, InstigatorName);
        if (!Instigator)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Instigator actor not found in PIE world: %s"), *InstigatorName),
                EMCPErrorCode::ActorNotFound,
                TEXT("Either omit the instigator (uses a blank context) or verify the actor name."));
        }
    }

    // Build the effect context. If we have an instigator, route through the
    // instigator's ASC to populate the context; otherwise call MakeEffectContext
    // on the target ASC for a self-applied effect.
    UAbilitySystemComponent* SourceASC = nullptr;
    if (Instigator)
    {
        SourceASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Instigator);
    }
    if (!SourceASC)
    {
        SourceASC = TargetASC;
    }
    FGameplayEffectContextHandle ContextHandle = SourceASC->MakeEffectContext();
    if (Instigator)
    {
        ContextHandle.AddInstigator(Instigator, /*EffectCauser=*/Instigator);
    }

    UGameplayEffect* EffectCDO = EffectClass->GetDefaultObject<UGameplayEffect>();
    FActiveGameplayEffectHandle Handle = TargetASC->ApplyGameplayEffectToTarget(
        EffectCDO,
        TargetASC,
        static_cast<float>(Level),
        ContextHandle);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("target_actor"), TargetActorName);
    ResultObj->SetStringField(TEXT("effect_class"), EffectClass->GetPathName());
    ResultObj->SetNumberField(TEXT("level"), Level);
    if (Instigator)
    {
        ResultObj->SetStringField(TEXT("instigator"), InstigatorName);
    }
    ResultObj->SetBoolField(TEXT("handle_valid"), Handle.IsValid());
    ResultObj->SetStringField(TEXT("world"), PIEWorld->GetPathName());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPGASCommands::HandleAttributeSetCreate(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, AssetName, FullAssetPath, ResolveError;
    if (!ResolveGASDestination(Params, PackagePath, AssetName, FullAssetPath, ResolveError))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            ResolveError,
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass {path:'/Game/GAS/AttributeSets', name:'AS_Foo'} or {asset_path:'/Game/GAS/AttributeSets/AS_Foo'}."));
    }
    if (UEditorAssetLibrary::DoesAssetExist(FullAssetPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s"), *FullAssetPath),
            EMCPErrorCode::NameCollision,
            TEXT("No silent overwrite. Pick a different path or delete the existing asset first."));
    }

    // Per doc 10: Blueprintable AttributeSet wrapper for prototyping. UAttributeSet
    // is annotated `Blueprintable` so the standard UBlueprintFactory path works.
    // Production AttributeSets are typically C++ — this surface explicitly flags
    // the result as `is_scaffolding` per the doc invariant ("the doc states this
    // so the agent doesn't propose Blueprint-only attribute design for shipped
    // systems").
    FString ParentClassPath;
    Params->TryGetStringField(TEXT("parent_class"), ParentClassPath);
    UClass* ParentClass = ResolveBPParent(ParentClassPath, UAttributeSet::StaticClass());
    if (!ParentClass)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve parent class: %s"), *ParentClassPath),
            EMCPErrorCode::ClassNotLoaded,
            TEXT("Pass a UAttributeSet subclass — script path, asset path, or short name. Defaults to UAttributeSet itself."));
    }
    if (!ParentClass->IsChildOf(UAttributeSet::StaticClass()))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Parent class is not a UAttributeSet subclass: %s"), *ParentClass->GetPathName()),
            EMCPErrorCode::UnsupportedClass,
            TEXT("gas_attributeset_create only creates UAttributeSet Blueprint scaffolding."));
    }

    UBlueprint* Created = CreateBlueprintViaFactory(ParentClass, AssetName, PackagePath);
    if (!Created)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("UAssetTools::CreateAsset returned null for the AttributeSet Blueprint"),
            EMCPErrorCode::EngineBusy,
            TEXT("UE rejected the creation; check the editor log."));
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Created);
    Created->MarkPackageDirty();
    const bool bSaved = UEditorAssetLibrary::SaveAsset(FullAssetPath, /*bOnlyIfIsDirty=*/false);
    if (!bSaved)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("AttributeSet Blueprint created in memory but not saved to disk: %s"), *FullAssetPath),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — the save silently no-ops while PIE is active or under an unattended/headless editor, leaving the asset dirty but unwritten. Stop PIE and retry, or save the asset manually."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("path"), FullAssetPath);
    ResultObj->SetStringField(TEXT("class"), TEXT("/Script/Engine.Blueprint"));
    ResultObj->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
    // is_scaffolding flag — explicit signal to the agent that this surface is
    // for prototyping, not production. Per doc 10's invariant, production
    // AttributeSets are C++. Per-attribute initial-value seeding lands via
    // the existing `create_variable` tool which adds NewVariables to a BP.
    ResultObj->SetBoolField(TEXT("is_scaffolding"), true);
    ResultObj->SetStringField(TEXT("scaffolding_note"),
        TEXT("Blueprintable AttributeSets are valid for prototyping. Production AttributeSets are typically C++; consider migrating once the attribute layout stabilizes. Add attributes via create_variable on this BP."));
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}
