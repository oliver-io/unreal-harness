#include "Commands/MCPBlueprintCommands.h"
#include "Commands/MCPCommonUtils.h"
#include "JsonObjectConverter.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/BlueprintFactory.h"
#include "Factories/BlueprintInterfaceFactory.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimLayerInterface.h"
#include "UObject/Interface.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Engine/Engine.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UObject/Field.h"
#include "UObject/FieldPath.h"
#include "UObject/UObjectIterator.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Timeline.h"
#include "K2Node_EnhancedInputAction.h"
#include "InputAction.h"
#include "AnimGraphNode_TwoBoneIK.h"
#include "AnimGraphNode_ModifyBone.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_LinkedAnimGraph.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateEntryNode.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Factories/BlendSpaceFactoryNew.h"
#include "Factories/BlendSpaceFactory1D.h"
#include "Factories/AimOffsetBlendSpaceFactoryNew.h"
#include "Factories/AimOffsetBlendSpaceFactory1D.h"
#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace1D.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimData/BoneMaskFilter.h"
#include "AnimNodes/AnimNode_LayeredBoneBlend.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"

FMCPBlueprintCommands::FMCPBlueprintCommands()
{
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("bp_create_blueprint"))
    {
        return HandleCreateBlueprint(Params);
    }
    else if (CommandType == TEXT("bp_set_default_value"))
    {
        return HandleSetBlueprintDefaultValue(Params);
    }
    else if (CommandType == TEXT("bp_add_component"))
    {
        return HandleAddComponentToBlueprint(Params);
    }
    else if (CommandType == TEXT("bp_set_component_transform"))
    {
        return HandleSetComponentTransform(Params);
    }
    else if (CommandType == TEXT("bp_set_component_property"))
    {
        return HandleSetComponentProperty(Params);
    }
    else if (CommandType == TEXT("bp_set_class_replication"))
    {
        return HandleSetClassReplication(Params);
    }
    else if (CommandType == TEXT("bp_set_event_replication"))
    {
        return HandleSetEventReplication(Params);
    }
    else if (CommandType == TEXT("physics_set_properties"))
    {
        return HandleSetPhysicsProperties(Params);
    }
    else if (CommandType == TEXT("bp_compile"))
    {
        return HandleCompileBlueprint(Params);
    }
    else if (CommandType == TEXT("mesh_set_static_mesh_properties"))
    {
        return HandleSetStaticMeshProperties(Params);
    }
    else if (CommandType == TEXT("spawn_blueprint_actor"))
    {
        return HandleSpawnBlueprintActor(Params);
    }
    else if (CommandType == TEXT("mesh_set_mesh_material_color"))
    {
        return HandleSetMeshMaterialColor(Params);
    }
    // Material management commands
    else if (CommandType == TEXT("material_get_available"))
    {
        return HandleGetAvailableMaterials(Params);
    }
    else if (CommandType == TEXT("material_apply_to_actor"))
    {
        return HandleApplyMaterialToActor(Params);
    }
    else if (CommandType == TEXT("material_apply_to_blueprint"))
    {
        return HandleApplyMaterialToBlueprint(Params);
    }
    else if (CommandType == TEXT("mesh_set_static_mesh_material"))
    {
        return HandleSetStaticMeshMaterial(Params);
    }
    else if (CommandType == TEXT("mesh_get_actor_material_info"))
    {
        return HandleGetActorMaterialInfo(Params);
    }
    else if (CommandType == TEXT("get_blueprint_material_info"))
    {
        return HandleGetBlueprintMaterialInfo(Params);
    }
    // Blueprint analysis commands
    else if (CommandType == TEXT("bp_read"))
    {
        return HandleReadBlueprintContent(Params);
    }
    else if (CommandType == TEXT("bp_inspect"))
    {
        return HandleAnalyzeBlueprintGraph(Params);
    }
    else if (CommandType == TEXT("bp_get_variable_details"))
    {
        return HandleGetBlueprintVariableDetails(Params);
    }
    else if (CommandType == TEXT("bp_get_function_details"))
    {
        return HandleGetBlueprintFunctionDetails(Params);
    }
    else if (CommandType == TEXT("reflection_class_properties"))
    {
        return HandleGetClassProperties(Params);
    }
    else if (CommandType == TEXT("get_mesh_bounds"))
    {
        return HandleGetMeshBounds(Params);
    }
    else if (CommandType == TEXT("bp_list_graphs"))
    {
        return HandleListBlueprintGraphs(Params);
    }
    else if (CommandType == TEXT("bp_reparent"))
    {
        return HandleReparentBlueprint(Params);
    }
    // Animation asset commands
    else if (CommandType == TEXT("anim_list_skeletons"))
    {
        return HandleListSkeletons(Params);
    }
    else if (CommandType == TEXT("anim_list_sequences"))
    {
        return HandleListAnimSequences(Params);
    }
    else if (CommandType == TEXT("anim_blend_space_create"))
    {
        return HandleCreateBlendSpace(Params);
    }
    else if (CommandType == TEXT("anim_blend_space_add_sample"))
    {
        return HandleAddBlendSpaceSample(Params);
    }
    else if (CommandType == TEXT("anim_blend_space_remove_sample"))
    {
        return HandleRemoveBlendSpaceSample(Params);
    }
    else if (CommandType == TEXT("anim_sequence_set_property"))
    {
        return HandleSetAnimSequenceProperty(Params);
    }
    // Skeleton socket commands
    else if (CommandType == TEXT("anim_skeleton_list_sockets"))
    {
        return HandleListSkeletonSockets(Params);
    }
    else if (CommandType == TEXT("anim_skeleton_add_socket"))
    {
        return HandleAddSkeletonSocket(Params);
    }
    else if (CommandType == TEXT("anim_skeleton_modify_socket"))
    {
        return HandleModifySkeletonSocket(Params);
    }
    else if (CommandType == TEXT("anim_skeleton_remove_socket"))
    {
        return HandleRemoveSkeletonSocket(Params);
    }
    // Animation asset discovery
    else if (CommandType == TEXT("bp_list_node_pins"))
    {
        return HandleListNodePins(Params);
    }
    else if (CommandType == TEXT("anim_list_blend_spaces"))
    {
        return HandleListBlendSpaces(Params);
    }
    else if (CommandType == TEXT("anim_blend_space_read"))
    {
        return HandleReadBlendSpace(Params);
    }
    else if (CommandType == TEXT("anim_list_blueprints"))
    {
        return HandleListAnimBlueprints(Params);
    }
    else if (CommandType == TEXT("anim_list_montages"))
    {
        return HandleListAnimMontages(Params);
    }
    else if (CommandType == TEXT("anim_list_layer_interfaces"))
    {
        return HandleListAnimLayerInterfaces(Params);
    }
    else if (CommandType == TEXT("anim_montage_read"))
    {
        return HandleReadAnimMontage(Params);
    }

    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown blueprint command: %s"), *CommandType),
        EMCPErrorCode::InvalidArgument,
        TEXT("`command` must be one of the supported blueprint commands: create_blueprint, add_component_to_blueprint, set_physics_properties, compile_blueprint, spawn_blueprint_actor, set_static_mesh_properties, set_mesh_material_color, get_available_materials, apply_material_to_actor, apply_material_to_blueprint, set_static_mesh_material, get_actor_material_info, get_blueprint_material_info, read_blueprint_content, analyze_blueprint_graph, get_blueprint_variable_details, get_blueprint_function_details, get_mesh_bounds, list_blueprint_graphs, reparent_blueprint, list_skeletons, list_anim_sequences, create_blend_space, add_blend_space_sample, remove_blend_space_sample, set_anim_sequence_property, list_skeleton_sockets, add_skeleton_socket, modify_skeleton_socket, remove_skeleton_socket, list_node_pins, list_blend_spaces, read_blend_space, list_anim_blueprints, list_anim_montages, list_anim_layer_interfaces, read_anim_montage, get_class_properties, set_blueprint_default_value."));
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleCreateBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    // ── Required: name ────────────────────────────────────────────────────
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`name` is required (string). The semantics depend on the handler — typically the new asset's short name (`MyBP`) or full asset path (`/Game/Folder/MyBP`). Path-form auto-creates intermediate folders."));
    }

    // ── Path handling ─────────────────────────────────────────────────────
    // Two accepted shapes:
    //   (a) "/Game/Foo/Bar/MyBP"  → PackagePath="/Game/Foo/Bar", AssetName="MyBP"
    //   (b) "MyBP"                → PackagePath="/Game/Blueprints", AssetName="MyBP"
    //                               (legacy default kept for back-compat)
    // IAssetTools::CreateAsset auto-creates intermediate folders for (a).
    FString PackagePath;
    FString AssetName;
    if (BlueprintName.StartsWith(TEXT("/")))
    {
        int32 LastSlash = INDEX_NONE;
        BlueprintName.FindLastChar(TEXT('/'), LastSlash);
        if (LastSlash <= 0)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Malformed asset path: '%s'. Expected '/Game/Folder/AssetName'."), *BlueprintName),
                EMCPErrorCode::InvalidPath,
                TEXT("Path-form `name` must include at least one folder segment between the leading `/` and the asset name, e.g. `/Game/Blueprints/BP_MyBP`. A bare `/MyBP` is not valid — pass `MyBP` instead (defaults to `/Game/Blueprints`) or supply the full path."));
        }
        PackagePath = BlueprintName.Left(LastSlash);
        AssetName = BlueprintName.Mid(LastSlash + 1);
    }
    else
    {
        PackagePath = TEXT("/Game/Blueprints");
        AssetName = BlueprintName;
    }

    if (UEditorAssetLibrary::DoesAssetExist(PackagePath / AssetName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s/%s"), *PackagePath, *AssetName),
            EMCPErrorCode::NameCollision,
            TEXT("An asset already exists at the target path. Pick a different `name`, or call `delete_asset` on the existing asset first. Use `read_blueprint_content` to inspect the existing asset before deciding."));
    }

    // ── Parent class resolution (three strategies) ────────────────────────
    //   1. Fully-qualified path (/Script/Module.Class) → LoadClass directly
    //   2. Short name → iterate UClass objects matching GetName() with/without
    //      a UE prefix stripped (accepts "Pawn", "APawn", "ForestAnimalCharacter"
    //      — all three resolve to the same class)
    //   3. Hardcoded Engine/Game-module path fallback (legacy path for
    //      back-compat with pre-fix callers)
    //
    // Pre-fix behavior: unresolved parent class silently fell back to AActor and
    // returned status=success, so a typo in parent_class produced a broken
    // blueprint with DefaultSceneRoot and no inherited components. We now
    // return an explicit error for unresolved parent classes.
    FString ParentClass;
    Params->TryGetStringField(TEXT("parent_class"), ParentClass);

    UClass* SelectedParentClass = AActor::StaticClass();

    if (!ParentClass.IsEmpty())
    {
        UClass* FoundClass = nullptr;

        if (ParentClass.Contains(TEXT("/")) || ParentClass.Contains(TEXT(".")))
        {
            FoundClass = LoadClass<UObject>(nullptr, *ParentClass);
        }
        else
        {
            FString Stripped = ParentClass;
            if (Stripped.Len() > 1 &&
                (Stripped[0] == TEXT('U') || Stripped[0] == TEXT('A')) &&
                FChar::IsUpper(Stripped[1]))
            {
                Stripped = Stripped.RightChop(1);
            }

            for (TObjectIterator<UClass> It; It; ++It)
            {
                UClass* Cls = *It;
                const FString CName = Cls->GetName();
                if (CName == ParentClass || CName == Stripped)
                {
                    FoundClass = Cls;
                    break;
                }
            }

            if (!FoundClass)
            {
                const FString ClassWithAPrefix = ParentClass.StartsWith(TEXT("A"))
                    ? ParentClass : (TEXT("A") + ParentClass);
                FoundClass = LoadClass<UObject>(nullptr,
                    *FString::Printf(TEXT("/Script/Engine.%s"), *ClassWithAPrefix));
                if (!FoundClass)
                {
                    FoundClass = LoadClass<UObject>(nullptr,
                        *FString::Printf(TEXT("/Script/Game.%s"), *ClassWithAPrefix));
                }
            }
        }

        if (!FoundClass)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(
                    TEXT("Could not resolve parent_class '%s'. Use a short class name "
                         "(e.g. 'Pawn', 'Character', 'AnimLayerInterface'), a UE-prefixed short "
                         "name (e.g. 'APawn', 'UAnimInstance'), or a fully-qualified script path "
                         "(e.g. '/Script/MyGame.MyCharacter')."),
                    *ParentClass),
                EMCPErrorCode::ClassNotLoaded,
                TEXT("`parent_class` did not resolve via the three lookup strategies (FQN path, short name with U/A prefix strip, Engine/Game fallback). The class must be loaded — for C++ classes, ensure the owning module is loaded; for BP parents, pass the BP's GeneratedClass path."));
        }

        SelectedParentClass = FoundClass;
        UE_LOG(LogUnrealMCP, Log, TEXT("create_blueprint: resolved parent_class '%s' to %s"),
            *ParentClass, *FoundClass->GetName());
    }

    // ── Optional: target_skeleton (required for AnimInstance / AnimLayerInterface)
    FString TargetSkeletonPath;
    Params->TryGetStringField(TEXT("target_skeleton"), TargetSkeletonPath);

    USkeleton* ResolvedSkeleton = nullptr;
    if (!TargetSkeletonPath.IsEmpty())
    {
        ResolvedSkeleton = LoadObject<USkeleton>(nullptr, *TargetSkeletonPath);
        if (!ResolvedSkeleton)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("target_skeleton '%s' did not resolve to a USkeleton asset."), *TargetSkeletonPath),
                EMCPErrorCode::AssetNotFound,
                TEXT("`target_skeleton` must be a full asset path to a USkeleton (e.g. `/Game/Characters/MyChar_Skeleton`). Use `list_skeletons` to discover."));
        }
    }

    // ── Factory dispatch by parent class ──────────────────────────────────
    // Order matters: AActor and UAnimInstance both descend from UObject, but
    // AnimInstance has dedicated factory machinery (skeleton + AnimGraph), and
    // UAnimLayerInterface is a singleton parent that drives BPTYPE_Interface
    // mode in UAnimLayerInterfaceFactory. UInterface is the catch-all for
    // non-anim Blueprint Interfaces.
    UFactory* SelectedFactory = nullptr;
    UClass* AssetClass = UBlueprint::StaticClass();
    FString DispatchedFactory;
    FString DispatchedAssetClass = TEXT("Blueprint");
    FString BlueprintTypeStr = TEXT("Normal");

    if (SelectedParentClass == UAnimLayerInterface::StaticClass())
    {
        if (!ResolvedSkeleton)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("AnimLayerInterface parent requires target_skeleton; none provided. "
                     "Pass a skeleton path (e.g. '/Game/Characters/Hero/SKEL_Hero')."),
                EMCPErrorCode::InvalidArgument,
                TEXT("Creating a Blueprint with UAnimLayerInterface parent requires `target_skeleton` because UAnimLayerInterfaceFactory binds the skeleton at factory time. Use `list_skeletons` to discover. Without it, the factory returns nullptr."));
        }
        UAnimLayerInterfaceFactory* F = NewObject<UAnimLayerInterfaceFactory>();
        F->TargetSkeleton = ResolvedSkeleton;
        // BlueprintType = BPTYPE_Interface set in UAnimLayerInterfaceFactory ctor.
        SelectedFactory = F;
        AssetClass = UAnimBlueprint::StaticClass();
        DispatchedFactory = TEXT("UAnimLayerInterfaceFactory");
        DispatchedAssetClass = TEXT("AnimBlueprint");
        BlueprintTypeStr = TEXT("Interface");
    }
    else if (SelectedParentClass->IsChildOf(UAnimInstance::StaticClass()))
    {
        if (!ResolvedSkeleton)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("UAnimInstance-derived parent requires target_skeleton; none provided. "
                     "Pass a skeleton path (e.g. '/Game/Characters/Hero/SKEL_Hero')."),
                EMCPErrorCode::InvalidArgument,
                TEXT("Creating a Blueprint with UAnimInstance-derived parent requires `target_skeleton` because UAnimBlueprintFactory binds the skeleton at factory time. Use `list_skeletons` to discover."));
        }
        UAnimBlueprintFactory* F = NewObject<UAnimBlueprintFactory>();
        F->ParentClass = SelectedParentClass;
        F->TargetSkeleton = ResolvedSkeleton;
        SelectedFactory = F;
        AssetClass = UAnimBlueprint::StaticClass();
        DispatchedFactory = TEXT("UAnimBlueprintFactory");
        DispatchedAssetClass = TEXT("AnimBlueprint");
    }
    else if (SelectedParentClass->IsChildOf(UInterface::StaticClass()))
    {
        // Generic Blueprint Interface (non-anim). The factory hardcodes its
        // own ParentClass to UInterface::StaticClass() in its ctor, so we
        // don't override it — passing an arbitrary IInterface subclass would
        // be ignored downstream. Treat all UInterface-rooted parents as a
        // request to author a fresh BlueprintInterface from UInterface.
        UBlueprintInterfaceFactory* F = NewObject<UBlueprintInterfaceFactory>();
        SelectedFactory = F;
        AssetClass = UBlueprint::StaticClass();
        DispatchedFactory = TEXT("UBlueprintInterfaceFactory");
        DispatchedAssetClass = TEXT("Blueprint");
        BlueprintTypeStr = TEXT("Interface");
    }
    else
    {
        // Fallback: generic UBlueprintFactory. Covers AActor-derived parents
        // (the original behavior of this tool), UBlueprintFunctionLibrary,
        // and any other UObject-rooted class with a KismetCompiler-registered
        // BP type pair. KismetCompiler picks the UBlueprint subclass and
        // UBlueprintGeneratedClass from ParentClass inside FactoryCreateNew,
        // so we don't need a dedicated branch per BP-able UObject root.
        //
        // NOTE: UUserWidget parents would dispatch to UWidgetBlueprintFactory
        // here ideally, but that factory lives in UMGEditor and pulling that
        // module into UnrealMCP.Build.cs is deferred (see
        // docs/todo/MCP_ANIMLAYERINTERFACE.md "Out of scope"). Falling through
        // to UBlueprintFactory will fail CanCreateBlueprintOfClass below for
        // UUserWidget anyway, returning a clear error instead of producing a
        // half-broken widget BP.
        if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(SelectedParentClass))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(
                    TEXT("Cannot create a Blueprint based on parent_class '%s' "
                         "(FKismetEditorUtilities::CanCreateBlueprintOfClass returned false). "
                         "Common causes: class is marked NotBlueprintable, abstract without "
                         "Blueprintable spec, or has no registered BP type pair. "
                         "If this is a UUserWidget subclass, the Widget BP factory is not "
                         "yet wired in UnrealMCP — see docs/todo/MCP_ANIMLAYERINTERFACE.md."),
                    *SelectedParentClass->GetName()),
                EMCPErrorCode::UnsupportedClass,
                TEXT("The resolved parent class is not BP-instantiable per FKismetEditorUtilities::CanCreateBlueprintOfClass. UUserWidget parents specifically are deferred (UMG factory wiring pending). For other rejects, verify the class is marked `Blueprintable` in its UCLASS macro."));
        }
        UBlueprintFactory* F = NewObject<UBlueprintFactory>();
        F->ParentClass = SelectedParentClass;
        SelectedFactory = F;
        AssetClass = UBlueprint::StaticClass();
        DispatchedFactory = TEXT("UBlueprintFactory");
        DispatchedAssetClass = TEXT("Blueprint");
    }

    // ── Create the asset via IAssetTools ──────────────────────────────────
    // CreateAsset handles package creation, intermediate folder creation,
    // asset registry notification, and editor refresh — equivalent to the
    // right-click "Create New Asset" path in the Content Browser.
    IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
    UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, AssetClass, SelectedFactory);
    if (!NewAsset)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(
                TEXT("IAssetTools::CreateAsset returned null for %s/%s (factory: %s, asset class: %s). "
                     "Check the editor log for factory-level errors."),
                *PackagePath, *AssetName, *DispatchedFactory, *DispatchedAssetClass),
            EMCPErrorCode::Internal,
            TEXT("AssetTools.CreateAsset returned nullptr despite a validated factory + asset class. Check the editor log for the factory-level error message. Common causes: destination path is read-only, AssetRegistry mid-scan, or factory rejected the asset for a runtime-specific reason."));
    }

    UBlueprint* NewBlueprint = Cast<UBlueprint>(NewAsset);
    if (!NewBlueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset created at %s but it is not a UBlueprint (got %s)."), *NewAsset->GetPathName(), *NewAsset->GetClass()->GetName()),
            EMCPErrorCode::Internal,
            TEXT("The factory created an asset but its class is not UBlueprint. This indicates a factory misconfiguration in the dispatch — the resolved factory and the cast type don't agree. Report as a bug; the asset path is in the error message for cleanup."));
    }

    UE_LOG(LogUnrealMCP, Log,
        TEXT("create_blueprint: created %s via %s (asset class %s, parent %s)"),
        *NewBlueprint->GetPathName(), *DispatchedFactory,
        *DispatchedAssetClass, *SelectedParentClass->GetPathName());

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("path"), NewBlueprint->GetPathName());
    ResultObj->SetStringField(TEXT("created_asset_path"), NewBlueprint->GetPathName());
    ResultObj->SetStringField(TEXT("asset_class"), DispatchedAssetClass);
    ResultObj->SetStringField(TEXT("blueprint_type"), BlueprintTypeStr);
    ResultObj->SetStringField(TEXT("parent_class"), SelectedParentClass->GetPathName());
    ResultObj->SetStringField(TEXT("factory"), DispatchedFactory);
    if (ResolvedSkeleton)
    {
        ResultObj->SetStringField(TEXT("target_skeleton"), ResolvedSkeleton->GetPathName());
    }
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleAddComponentToBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required (string). Accepts either a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints."));
    }

    FString ComponentType;
    if (!Params->TryGetStringField(TEXT("component_type"), ComponentType))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'type' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`type` is required (string, the component class short-name — e.g. `StaticMeshComponent`, `BoxComponent`, `SkeletalMeshComponent`). The U-prefix is auto-handled. Use `list_node_pins` or `get_class_properties` on a candidate to verify."));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`name` is required (string). The semantics depend on the handler — typically the new asset's short name (`MyBP`) or full asset path (`/Game/Folder/MyBP`). Path-form auto-creates intermediate folders."));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_name` did not resolve to a Blueprint via the usual lookup (short name against `/Game/Blueprints/`, then full asset path). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints. Names are case-sensitive."));
    }

    // Resolve the component class. Pre-fix, this used FindObject<UClass>(nullptr, Name)
    // which only searched the transient package — engine built-ins (StaticMeshComponent,
    // etc.) worked because their UClass objects happen to be findable that way, but
    // project-module classes (e.g. a UMyComponent defined in the project's game module)
    // were never discovered and always returned "Unknown component type".
    //
    // Two-strategy resolver: full path first (accepts /Script/Module.Class and
    // /Game/BP_Foo.BP_Foo_C), then UClass iteration for short names. Names are
    // matched against GetName() with a 'U' prefix optionally stripped, and against
    // the same with a "Component" suffix appended (so "StaticMesh" → "StaticMeshComponent").
    UClass* ComponentClass = nullptr;

    if (ComponentType.Contains(TEXT("/")) || ComponentType.Contains(TEXT(".")))
    {
        ComponentClass = LoadClass<UActorComponent>(nullptr, *ComponentType);
    }
    else
    {
        // Build the set of candidate short-name matches to test against UClass::GetName().
        // UClass::GetName() stores names WITHOUT the 'U' prefix, so "UHerdMembershipComponent"
        // → GetName() == "HerdMembershipComponent".
        TArray<FString> Candidates;
        Candidates.Add(ComponentType);

        FString Stripped = ComponentType;
        if (Stripped.Len() > 1 && Stripped[0] == TEXT('U') && FChar::IsUpper(Stripped[1]))
        {
            Stripped = Stripped.RightChop(1);
            Candidates.Add(Stripped);
        }

        if (!ComponentType.EndsWith(TEXT("Component")))
        {
            Candidates.Add(ComponentType + TEXT("Component"));
            if (Stripped != ComponentType)
            {
                Candidates.Add(Stripped + TEXT("Component"));
            }
        }

        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Cls = *It;
            if (!Cls->IsChildOf(UActorComponent::StaticClass())) continue;
            if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;

            const FString CName = Cls->GetName();
            for (const FString& Candidate : Candidates)
            {
                if (CName == Candidate)
                {
                    ComponentClass = Cls;
                    break;
                }
            }
            if (ComponentClass) break;
        }
    }

    // Verify that the class is a valid component type
    if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(
                TEXT("Unknown component type: '%s'. Pass a class name (e.g. 'StaticMeshComponent', "
                     "'HerdMembershipComponent') or a fully-qualified path "
                     "(e.g. '/Script/MyGame.MyComponent')."),
                *ComponentType),
            EMCPErrorCode::ClassNotLoaded,
            TEXT("`component_type` did not resolve to a UActorComponent subclass. Accepts short name (`StaticMeshComponent`), U-prefixed (`UStaticMeshComponent`), or FQN path. For custom game-module components, ensure the module is loaded."));
    }

    // Guard the SimpleConstructionScript before dereferencing it. A Blueprint
    // whose SCS is null (some non-Actor / data-only Blueprint kinds) would
    // null-deref on ->CreateNode below → EXCEPTION_ACCESS_VIOLATION. Refuse
    // cleanly instead. (Actor Blueprints normally have a valid SCS.)
    if (!Blueprint->SimpleConstructionScript)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint '%s' has no SimpleConstructionScript — cannot add components"), *BlueprintName),
            EMCPErrorCode::UnsupportedClass,
            TEXT("This Blueprint has a null SimpleConstructionScript (it is not an Actor/Component-hosting Blueprint, e.g. a data-only or function-library Blueprint). Components can only be added to Blueprints with a construction script — verify the Blueprint is an Actor subclass."));
    }

    // GAP-014: optional parent_component nesting. Resolve the parent SCS node up front
    // so we can attach under it instead of always parenting to the root.
    FString ParentComponentName;
    Params->TryGetStringField(TEXT("parent_component"), ParentComponentName);
    USCS_Node* ParentNode = nullptr;
    if (!ParentComponentName.IsEmpty())
    {
        for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (Node && Node->GetVariableName().ToString() == ParentComponentName)
            {
                ParentNode = Node;
                break;
            }
        }
        if (!ParentNode)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Parent component not found: %s"), *ParentComponentName),
                EMCPErrorCode::NodeNotFound,
                TEXT("`parent_component` did not match any existing component on this Blueprint. Add the parent first, or omit `parent_component` to attach at the root. Use `bp_list_components` to enumerate."));
        }
    }

    // GAP-020: a re-add with the SAME `component_name` must UPDATE the existing
    // component, not silently spawn a disambiguated duplicate. CreateNode →
    // GenerateNewComponentName auto-renames on ANY name collision (Foo → Foo1)
    // (Engine/Source/Runtime/Engine/Private/SimpleConstructionScript.cpp:1356),
    // so we detect the collision ourselves first and decide add-vs-update.
    USCS_Node* ExistingNode = Blueprint->SimpleConstructionScript->FindSCSNode(FName(*ComponentName));
    bool bUpdated = false;
    USCS_Node* NewNode = nullptr;

    if (ExistingNode)
    {
        // A component already carries this name. If its class differs, refuse:
        // silently swapping the template out would be destructive (lose the
        // existing component). This is an update path, not a replace path.
        if (ExistingNode->ComponentClass != ComponentClass)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(
                    TEXT("Component '%s' already exists on '%s' with a different class (%s != requested %s)"),
                    *ComponentName, *BlueprintName,
                    ExistingNode->ComponentClass ? *ExistingNode->ComponentClass->GetName() : TEXT("None"),
                    *ComponentClass->GetName()),
                EMCPErrorCode::NameCollision,
                TEXT("A component with this `component_name` already exists but is a DIFFERENT `component_type`. Re-adding only updates in place when the class matches — it is not a replace path. Remove it with `bp_remove_component` first, or choose a different `component_name`."));
        }
        // Same name + same class → treat this call as an in-place update of the
        // existing component (apply transform / component_properties / re-parent)
        // instead of creating a duplicate.
        NewNode = ExistingNode;
        bUpdated = true;
    }
    else
    {
        NewNode = Blueprint->SimpleConstructionScript->CreateNode(ComponentClass, *ComponentName);
    }

    if (NewNode)
    {
        if (bUpdated)
        {
            // Transaction-safety for the in-place mutation of an existing template.
            Blueprint->Modify();
            if (NewNode->ComponentTemplate)
            {
                NewNode->ComponentTemplate->Modify();
            }
        }

        // Set transform if provided
        USceneComponent* SceneComponent = Cast<USceneComponent>(NewNode->ComponentTemplate);
        if (SceneComponent)
        {
            if (Params->HasField(TEXT("location")))
            {
                SceneComponent->SetRelativeLocation(FMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")));
            }
            if (Params->HasField(TEXT("rotation")))
            {
                SceneComponent->SetRelativeRotation(FMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation")));
            }
            if (Params->HasField(TEXT("scale")))
            {
                SceneComponent->SetRelativeScale3D(FMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale")));
            }
        }

        // GAP-012/020: apply `component_properties` to the component template. The map
        // was advertised in the schema but silently ignored — only transform was set.
        // Each entry is a (possibly dotted) property path resolved against the template.
        TArray<TSharedPtr<FJsonValue>> PropErrors;
        const TSharedPtr<FJsonObject>* PropsObj = nullptr;
        if (NewNode->ComponentTemplate && Params->TryGetObjectField(TEXT("component_properties"), PropsObj) && PropsObj && PropsObj->IsValid())
        {
            for (const auto& Pair : (*PropsObj)->Values)
            {
                FString PropErr;
                if (!FMCPCommonUtils::SetObjectPropertyByPath(NewNode->ComponentTemplate, Pair.Key, Pair.Value, PropErr))
                {
                    PropErrors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: %s"), *Pair.Key, *PropErr)));
                }
            }
        }

        // GAP-053: optional `skeletal_mesh` sets the mesh on a SkeletalMeshComponent
        // TEMPLATE through the engine's own Setter (SetSkeletalMeshAsset → SetSkeletalMesh),
        // which is the ONLY faithful way to give the template a non-null mesh. The canonical
        // UPROPERTY (USkinnedMeshComponent::SkeletalMeshAsset) is `Transient`, and the asset
        // is wired across SkinnedAsset / the deprecated SkeletalMesh alias only inside the
        // Setter — so a raw property write (`component_properties` → JsonValueToUProperty →
        // ImportText_Direct, which does NOT invoke the Setter) sets the transient field,
        // never reaches GetSkinnedAsset(), and isn't serialized. Setting it here makes the
        // spawned component REGISTER with a mesh, so its render state is created WITH a scene
        // proxy at registration. That closes the GAP-053 trap: a mesh first assigned at
        // runtime in BeginPlay renders nothing, because the null→valid swap's
        // FRenderStateRecreator no-ops when no render state existed yet (it recreates only
        // when bWasRenderStateCreated was true). UStaticMeshComponent has the equivalent
        // SetStaticMesh path via `set_static_mesh_properties`.
        FString SkeletalMeshPath;
        if (NewNode->ComponentTemplate && Params->TryGetStringField(TEXT("skeletal_mesh"), SkeletalMeshPath) && !SkeletalMeshPath.IsEmpty())
        {
            USkeletalMeshComponent* SkelComp = Cast<USkeletalMeshComponent>(NewNode->ComponentTemplate);
            if (!SkelComp)
            {
                PropErrors.Add(MakeShared<FJsonValueString>(FString::Printf(
                    TEXT("skeletal_mesh: component '%s' is a %s, not a USkeletalMeshComponent"),
                    *ComponentName,
                    NewNode->ComponentTemplate->GetClass() ? *NewNode->ComponentTemplate->GetClass()->GetName() : TEXT("None"))));
            }
            else if (USkeletalMesh* SkelMesh = Cast<USkeletalMesh>(UEditorAssetLibrary::LoadAsset(SkeletalMeshPath)))
            {
                SkelComp->SetSkeletalMeshAsset(SkelMesh);
            }
            else
            {
                PropErrors.Add(MakeShared<FJsonValueString>(FString::Printf(
                    TEXT("skeletal_mesh: '%s' did not resolve to a USkeletalMesh"), *SkeletalMeshPath)));
            }
        }

        // GAP-014: attach under the named parent if one was resolved, else the root.
        if (!bUpdated)
        {
            // Freshly created node — insert it into the SCS tree.
            if (ParentNode)
            {
                ParentNode->AddChildNode(NewNode);
            }
            else
            {
                Blueprint->SimpleConstructionScript->AddNode(NewNode);
            }
        }
        else if (ParentNode)
        {
            // GAP-020 update path: the node is already in the tree. Re-nest under
            // the requested parent only if it actually changed (preserving its own
            // subtree). With no `parent_component` we leave the hierarchy untouched.
            USCS_Node* CurrentParent = Blueprint->SimpleConstructionScript->FindParentNode(NewNode);
            if (CurrentParent != ParentNode)
            {
                Blueprint->SimpleConstructionScript->RemoveNode(NewNode);
                ParentNode->AddChildNode(NewNode);
            }
        }

        // Compile the blueprint
        FKismetEditorUtilities::CompileBlueprint(Blueprint);

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        // GAP-020: report the ACTUAL variable name. UE auto-disambiguates a duplicate
        // (Foo → Foo1), and the caller needs the real name to reference the component.
        const FString ActualName = NewNode->GetVariableName().IsNone()
            ? ComponentName : NewNode->GetVariableName().ToString();
        ResultObj->SetStringField(TEXT("component_name"), ActualName);
        if (ActualName != ComponentName)
        {
            ResultObj->SetStringField(TEXT("requested_name"), ComponentName);
        }
        // GAP-020: true when this call updated an existing same-named component
        // (same class) in place rather than creating a new one.
        ResultObj->SetBoolField(TEXT("updated"), bUpdated);
        ResultObj->SetStringField(TEXT("component_type"), ComponentType);
        if (!ParentComponentName.IsEmpty())
        {
            ResultObj->SetStringField(TEXT("parent_component"), ParentComponentName);
        }
        if (PropErrors.Num() > 0)
        {
            // Surface per-property failures without failing the whole add — the
            // component exists; some overrides didn't apply.
            ResultObj->SetArrayField(TEXT("property_errors"), PropErrors);
        }
        return ResultObj;
    }

    return FMCPCommonUtils::CreateErrorResponse(
        TEXT("Failed to add component to blueprint"),
        EMCPErrorCode::Internal,
        TEXT("FKismetEditorUtilities::AddDefaultSubobject (or the matching SCS node insert) returned no usable node. Common causes: the component class is abstract, not BP-instanceable, or already exists at the requested name. Check the editor log for the SCS-side error detail."));
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleSetPhysicsProperties(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required (string). Accepts either a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints."));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'component_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`component_name` is required (string, the component's UObject name on the Blueprint's SCS — e.g. `StaticMesh1`, `DefaultSceneRoot`). Use `read_blueprint_content` to enumerate the Blueprint's SCS nodes."));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_name` did not resolve to a Blueprint via the usual lookup (short name against `/Game/Blueprints/`, then full asset path). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints. Names are case-sensitive."));
    }

    // Guard the SimpleConstructionScript before iterating it. A Blueprint whose
    // SCS is null (data-only / interface / function-library / anim Blueprints) would
    // null-deref on ->GetAllNodes() below → EXCEPTION_ACCESS_VIOLATION. Refuse cleanly.
    if (!Blueprint->SimpleConstructionScript)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint '%s' has no SimpleConstructionScript — it has no components"), *BlueprintName),
            EMCPErrorCode::UnsupportedClass,
            TEXT("This Blueprint has a null SimpleConstructionScript (it is not an Actor/Component-hosting Blueprint, e.g. a data-only or function-library Blueprint). Use `read_blueprint_content` to inspect; verify the Blueprint is an Actor subclass."));
    }

    // Find the component
    USCS_Node* ComponentNode = nullptr;
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node && Node->GetVariableName().ToString() == ComponentName)
        {
            ComponentNode = Node;
            break;
        }
    }

    if (!ComponentNode)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Component not found: %s"), *ComponentName),
            EMCPErrorCode::NodeNotFound,
            TEXT("`component_name` did not match any SCS node on the Blueprint. Component names match the SCS variable name (case-sensitive), not the display label. Use `read_blueprint_content` to enumerate the Blueprint's components."));
    }

    UPrimitiveComponent* PrimComponent = Cast<UPrimitiveComponent>(ComponentNode->ComponentTemplate);
    if (!PrimComponent)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Component is not a primitive component"),
            EMCPErrorCode::UnsupportedClass,
            TEXT("The resolved component is not a UPrimitiveComponent — this operation only works on primitive components (StaticMesh, SkeletalMesh, BoxComponent, SphereComponent, etc., which have physics + collision). Scene-only components (SceneComponent, USceneCaptureComponent, etc.) don't support it."));
    }

    // Set physics properties
    if (Params->HasField(TEXT("simulate_physics")))
    {
        PrimComponent->SetSimulatePhysics(Params->GetBoolField(TEXT("simulate_physics")));
    }

    if (Params->HasField(TEXT("mass")))
    {
        float Mass = Params->GetNumberField(TEXT("mass"));
        // In UE5.5, use proper overrideMass instead of just scaling
        PrimComponent->SetMassOverrideInKg(NAME_None, Mass);
        UE_LOG(LogUnrealMCP, Display, TEXT("Set mass for component %s to %f kg"), *ComponentName, Mass);
    }

    if (Params->HasField(TEXT("linear_damping")))
    {
        PrimComponent->SetLinearDamping(Params->GetNumberField(TEXT("linear_damping")));
    }

    if (Params->HasField(TEXT("angular_damping")))
    {
        PrimComponent->SetAngularDamping(Params->GetNumberField(TEXT("angular_damping")));
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("component"), ComponentName);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleCompileBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required (string). Accepts either a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints."));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_name` did not resolve to a Blueprint via the usual lookup (short name against `/Game/Blueprints/`, then full asset path). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints. Names are case-sensitive."));
    }

    // Compile the blueprint, capturing the compiler results log so we can surface the
    // actual errors/warnings. GAP-054: the old path checked only Blueprint->Status and
    // returned {"compiled":true} even when the compiler logged hard [Compiler] Errors
    // (some node-level errors don't flip Status to BS_Error), so an agent believed a
    // broken graph compiled clean. Now we inspect FCompilerResultsLog directly.
    FCompilerResultsLog Results;
    Results.bSilentMode = true;
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);

    // Collect the per-message text (errors first, then warnings) for the response.
    TArray<TSharedPtr<FJsonValue>> ErrorMsgs;
    TArray<TSharedPtr<FJsonValue>> WarningMsgs;
    for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
    {
        const FString Text = Msg->ToText().ToString();
        switch (Msg->GetSeverity())
        {
        case EMessageSeverity::Error:
            ErrorMsgs.Add(MakeShared<FJsonValueString>(Text));
            break;
        case EMessageSeverity::Warning:
        case EMessageSeverity::PerformanceWarning:
            WarningMsgs.Add(MakeShared<FJsonValueString>(Text));
            break;
        default:
            break;
        }
    }

    const bool bHasErrors = (Results.NumErrors > 0) || (Blueprint->Status == BS_Error);

    if (bHasErrors)
    {
        // Return a structured error that carries the actual compiler messages so the
        // caller doesn't have to grep the editor log to learn what broke. The bridge
        // places this whole object under the envelope's `result`, so the fields below
        // surface as result.errors / result.num_errors etc.
        FString Joined;
        for (const TSharedPtr<FJsonValue>& E : ErrorMsgs)
        {
            Joined += (Joined.IsEmpty() ? TEXT("") : TEXT("; ")) + E->AsString();
        }

        TSharedPtr<FJsonObject> ErrResp = FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Compilation of %s failed: %d error(s)%s"),
                *BlueprintName, Results.NumErrors,
                Joined.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" — %s"), *Joined)),
            EMCPErrorCode::AssetCompileFailed,
            TEXT("The Blueprint compiled with errors (listed in `errors`). Fix the reported node/pin issues and recompile. Common causes: missing pin connections, type mismatches, dangling references after a refactor."));
        ErrResp->SetBoolField(TEXT("compiled"), false);
        ErrResp->SetNumberField(TEXT("num_errors"), Results.NumErrors);
        ErrResp->SetNumberField(TEXT("num_warnings"), Results.NumWarnings);
        ErrResp->SetArrayField(TEXT("errors"), ErrorMsgs);
        ErrResp->SetArrayField(TEXT("warnings"), WarningMsgs);
        return ErrResp;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("name"), BlueprintName);
    ResultObj->SetBoolField(TEXT("compiled"), true);
    ResultObj->SetNumberField(TEXT("num_warnings"), Results.NumWarnings);
    ResultObj->SetArrayField(TEXT("warnings"), WarningMsgs);

    // GAPS #6: optional one-shot save after a successful compile, so a caller can
    // collapse the common `bp_compile` → `asset_save` pair into a single bridge
    // round-trip. Defaults off (omitted) → byte-identical legacy behavior. The save
    // lives strictly inside this success branch: a failed compile returns above and
    // can never reach this point, so we never persist a broken Blueprint.
    bool bSave = false;
    Params->TryGetBoolField(TEXT("save"), bSave);
    if (bSave)
    {
        Blueprint->MarkPackageDirty();
        const bool bSaved = UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);
        ResultObj->SetBoolField(TEXT("saved"), bSaved);
        if (!bSaved)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Compiled %s but saving to disk failed"), *BlueprintName),
                EMCPErrorCode::Internal,
                TEXT("The Blueprint compiled successfully in-memory but UEditorAssetLibrary::SaveAsset returned false, so it was not written to disk (the package may be read-only, or saving is suppressed in the current context). The compiled result is unpersisted; retry the save once the cause is cleared."));
        }
    }
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleSpawnBlueprintActor(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("HandleSpawnBlueprintActor: Missing blueprint_name parameter"));
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required (string). Accepts either a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints."));
    }

    FString ActorName;
    if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("HandleSpawnBlueprintActor: Missing actor_name parameter"));
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'actor_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`actor_name` is required (string, the actor's UObject name or ActorLabel). Use `get_actors_in_level` or `find_actors_by_name` to discover."));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("HandleSpawnBlueprintActor: Blueprint not found: %s"), *BlueprintName);
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_name` did not resolve to a Blueprint via the usual lookup (short name against `/Game/Blueprints/`, then full asset path). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints. Names are case-sensitive."));
    }

    // Get transform parameters
    FVector Location(0.0f, 0.0f, 0.0f);
    FRotator Rotation(0.0f, 0.0f, 0.0f);

    if (Params->HasField(TEXT("location")))
    {
        Location = FMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = FMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
    }

    // Spawn the actor
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("HandleSpawnBlueprintActor: Failed to get editor world"));
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Failed to get editor world"),
            EMCPErrorCode::Internal,
            TEXT("GEditor->GetEditorWorldContext().World() returned null. The editor's world context is not yet initialized — typically only happens at editor startup or shutdown. Retry once the editor is fully loaded."));
    }

    FTransform SpawnTransform;
    SpawnTransform.SetLocation(Location);
    SpawnTransform.SetRotation(FQuat(Rotation));

    // Add a small delay to allow the engine to process the newly compiled class
    FPlatformProcess::Sleep(0.2f);

    AActor* NewActor = World->SpawnActor<AActor>(Blueprint->GeneratedClass, SpawnTransform);

    if (NewActor)
    {
        NewActor->SetActorLabel(*ActorName);
        TSharedPtr<FJsonObject> Result = FMCPCommonUtils::ActorToJsonObject(NewActor, true);
        return Result;
    }

    UE_LOG(LogUnrealMCP, Error, TEXT("HandleSpawnBlueprintActor: Failed to spawn blueprint actor"));
    return FMCPCommonUtils::CreateErrorResponse(
        TEXT("Failed to spawn blueprint actor"),
        EMCPErrorCode::Internal,
        TEXT("World->SpawnActor returned nullptr for the Blueprint's GeneratedClass. Common causes: spawn location inside collision (and `bNoCollisionFail` is off), the Blueprint is abstract, or PreSpawn validation rejected the actor. Try a clear `location` (e.g. `{x:0,y:0,z:200}`) and retry."));
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleSetStaticMeshProperties(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required (string). Accepts either a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints."));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'component_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`component_name` is required (string, the component's UObject name on the Blueprint's SCS — e.g. `StaticMesh1`, `DefaultSceneRoot`). Use `read_blueprint_content` to enumerate the Blueprint's SCS nodes."));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_name` did not resolve to a Blueprint via the usual lookup (short name against `/Game/Blueprints/`, then full asset path). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints. Names are case-sensitive."));
    }

    // Guard the SimpleConstructionScript before iterating it. A Blueprint whose
    // SCS is null (data-only / interface / function-library / anim Blueprints) would
    // null-deref on ->GetAllNodes() below → EXCEPTION_ACCESS_VIOLATION. Refuse cleanly.
    if (!Blueprint->SimpleConstructionScript)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint '%s' has no SimpleConstructionScript — it has no components"), *BlueprintName),
            EMCPErrorCode::UnsupportedClass,
            TEXT("This Blueprint has a null SimpleConstructionScript (it is not an Actor/Component-hosting Blueprint, e.g. a data-only or function-library Blueprint). Use `read_blueprint_content` to inspect; verify the Blueprint is an Actor subclass."));
    }

    // Find the component
    USCS_Node* ComponentNode = nullptr;
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node && Node->GetVariableName().ToString() == ComponentName)
        {
            ComponentNode = Node;
            break;
        }
    }

    if (!ComponentNode)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Component not found: %s"), *ComponentName),
            EMCPErrorCode::NodeNotFound,
            TEXT("`component_name` did not match any SCS node on the Blueprint. Component names match the SCS variable name (case-sensitive), not the display label. Use `read_blueprint_content` to enumerate the Blueprint's components."));
    }

    UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(ComponentNode->ComponentTemplate);
    if (!MeshComponent)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Component is not a static mesh component"),
            EMCPErrorCode::UnsupportedClass,
            TEXT("The resolved component is not a UStaticMeshComponent — this operation requires a static-mesh component to set the mesh / material. For SkeletalMeshComponents, use the skeletal-mesh-specific tools instead."));
    }

    // Set static mesh properties
    if (Params->HasField(TEXT("static_mesh")))
    {
        FString MeshPath = Params->GetStringField(TEXT("static_mesh"));
        UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(MeshPath));
        if (Mesh)
        {
            MeshComponent->SetStaticMesh(Mesh);
        }
    }

    if (Params->HasField(TEXT("material")))
    {
        FString MaterialPath = Params->GetStringField(TEXT("material"));
        UMaterialInterface* Material = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(MaterialPath));
        if (Material)
        {
            MeshComponent->SetMaterial(0, Material);
        }
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("component"), ComponentName);
    return ResultObj;
}

// Set the relative transform of an EXISTING component template on a Blueprint's SCS
// (location / rotation / scale — any subset). Fills the gap where bp_add_component
// could set a transform only at creation. Used to fit/orient a swapped mesh and
// re-seat attached components (e.g. a rider) without re-adding the component.
TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleSetComponentTransform(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required (short name or full asset path)."));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'component_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`component_name` is required (the SCS variable name, e.g. `BikeBody`). Use bp_list_components."));
    }

    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_name` did not resolve. Use list_assets asset_type='Blueprint'."));
    }

    if (!Blueprint->SimpleConstructionScript)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint '%s' has no SimpleConstructionScript"), *BlueprintName),
            EMCPErrorCode::UnsupportedClass,
            TEXT("Not an Actor/component-hosting Blueprint."));
    }

    USCS_Node* ComponentNode = nullptr;
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node && Node->GetVariableName().ToString() == ComponentName)
        {
            ComponentNode = Node;
            break;
        }
    }
    if (!ComponentNode)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Component not found: %s"), *ComponentName),
            EMCPErrorCode::NodeNotFound,
            TEXT("`component_name` did not match any SCS node (case-sensitive variable name). Use bp_list_components."));
    }

    USceneComponent* SceneComponent = Cast<USceneComponent>(ComponentNode->ComponentTemplate);
    if (!SceneComponent)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Component is not a scene component (has no transform)"),
            EMCPErrorCode::UnsupportedClass,
            TEXT("Only USceneComponent subclasses have a relative transform."));
    }

    bool bChanged = false;
    if (Params->HasField(TEXT("location")))
    {
        SceneComponent->SetRelativeLocation(FMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")));
        bChanged = true;
    }
    if (Params->HasField(TEXT("rotation")))
    {
        SceneComponent->SetRelativeRotation(FMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation")));
        bChanged = true;
    }
    if (Params->HasField(TEXT("scale")))
    {
        SceneComponent->SetRelativeScale3D(FMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale")));
        bChanged = true;
    }

    if (!bChanged)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("No transform fields provided"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Provide at least one of `location`, `rotation`, `scale`."));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    const FVector L = SceneComponent->GetRelativeLocation();
    const FRotator R = SceneComponent->GetRelativeRotation();
    const FVector S = SceneComponent->GetRelativeScale3D();
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("component"), ComponentName);
    TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
    Loc->SetNumberField(TEXT("x"), L.X); Loc->SetNumberField(TEXT("y"), L.Y); Loc->SetNumberField(TEXT("z"), L.Z);
    ResultObj->SetObjectField(TEXT("location"), Loc);
    TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
    Rot->SetNumberField(TEXT("pitch"), R.Pitch); Rot->SetNumberField(TEXT("yaw"), R.Yaw); Rot->SetNumberField(TEXT("roll"), R.Roll);
    ResultObj->SetObjectField(TEXT("rotation"), Rot);
    TSharedPtr<FJsonObject> Scl = MakeShared<FJsonObject>();
    Scl->SetNumberField(TEXT("x"), S.X); Scl->SetNumberField(TEXT("y"), S.Y); Scl->SetNumberField(TEXT("z"), S.Z);
    ResultObj->SetObjectField(TEXT("scale"), Scl);
    return ResultObj;
}

// Set ANY edit-exposed UPROPERTY on an existing component template of a Blueprint's
// SCS by reflection (e.g. SpringArm.TargetArmLength, SpringArm.SocketOffset,
// BoxComponent.BoxExtent, *.bVisible). Closes the gap where only transforms were
// settable on an existing component. Reuses FMCPCommonUtils::SetObjectProperty.
TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleSetComponentProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentName, PropertyName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
        return FMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name'"),
            EMCPErrorCode::InvalidArgument, TEXT("`blueprint_name` is required (short name or full asset path)."));
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
        return FMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name'"),
            EMCPErrorCode::InvalidArgument, TEXT("`component_name` is the SCS variable name (e.g. `SpringArm`). Use bp_list_components."));
    if (!Params->TryGetStringField(TEXT("property"), PropertyName))
        return FMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property'"),
            EMCPErrorCode::InvalidArgument, TEXT("`property` is the UPROPERTY name on the component (e.g. `TargetArmLength`)."));

    TSharedPtr<FJsonValue> ValueField = Params->TryGetField(TEXT("value"));
    if (!ValueField.IsValid())
        return FMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'value'"),
            EMCPErrorCode::InvalidArgument, TEXT("`value` is required (number/bool/string/array/object matching the property type)."));

    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
        return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
            EMCPErrorCode::AssetNotFound, TEXT("`blueprint_name` did not resolve."));
    if (!Blueprint->SimpleConstructionScript)
        return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint '%s' has no SimpleConstructionScript"), *BlueprintName),
            EMCPErrorCode::UnsupportedClass, TEXT("Not an Actor/component-hosting Blueprint."));

    USCS_Node* ComponentNode = nullptr;
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node && Node->GetVariableName().ToString() == ComponentName) { ComponentNode = Node; break; }
    }
    if (!ComponentNode || !ComponentNode->ComponentTemplate)
        return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Component not found: %s"), *ComponentName),
            EMCPErrorCode::NodeNotFound, TEXT("`component_name` did not match any SCS node. Use bp_list_components."));

    // GAP-013/025: support dotted property paths (e.g. "BodyInstance.bNotifyRigidBodyCollision",
    // "Settings.SomeStruct.Field") via the path-aware setter, not just top-level FProperties.
    FString WriteError;
    const bool bSet = FMCPCommonUtils::SetObjectPropertyByPath(ComponentNode->ComponentTemplate, PropertyName, ValueField, WriteError);
    if (!bSet)
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to set %s.%s: %s"), *ComponentName, *PropertyName, *WriteError),
            EMCPErrorCode::InvalidArgument,
            TEXT("The property name or value type didn't match the component's reflected UPROPERTY. Verify the exact property name (dotted paths into structs/sub-objects are allowed) and a compatible value shape."));

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("component"), ComponentName);
    ResultObj->SetStringField(TEXT("property"), PropertyName);
    ResultObj->SetBoolField(TEXT("set"), true);
    return ResultObj;
}

// GAP-055: set actor-class networking on a Blueprint CDO (bReplicates,
// bReplicateMovement, bAlwaysRelevant, NetCullDistanceSquared). bp_set_default_value
// targets BP-declared vars, not these inherited native AActor UPROPERTYs, so there
// was no MCP path to make a Blueprint replicate. Recompiles + saves.
TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleSetClassReplication(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
        return FMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name'"),
            EMCPErrorCode::InvalidArgument, TEXT("`blueprint_name` is required (short name or full asset path)."));

    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
        return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
            EMCPErrorCode::AssetNotFound, TEXT("`blueprint_name` did not resolve. Use list_assets asset_type='Blueprint'."));

    UClass* GenClass = Blueprint->GeneratedClass;
    if (!GenClass || !GenClass->IsChildOf(AActor::StaticClass()))
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint '%s' is not an Actor-derived class"), *BlueprintName),
            EMCPErrorCode::UnsupportedClass,
            TEXT("bp_set_class_replication only applies to Actor Blueprints (the net flags live on AActor). Compile the Blueprint first if GeneratedClass is null."));

    AActor* CDO = Cast<AActor>(GenClass->GetDefaultObject());
    if (!CDO)
        return FMCPCommonUtils::CreateErrorResponse(TEXT("Could not resolve the Actor CDO"),
            EMCPErrorCode::Internal, TEXT("GeneratedClass->GetDefaultObject() returned null or non-Actor. Recompile the Blueprint and retry."));

    Blueprint->Modify();
    CDO->Modify();
    CDO->PreEditChange(nullptr);

    bool bChanged = false;
    TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();

    bool bReplicates = false;
    if (Params->TryGetBoolField(TEXT("replicates"), bReplicates))
    {
        CDO->SetReplicates(bReplicates);  // sets bReplicates + keeps RemoteRole consistent
        bChanged = true;
        Applied->SetBoolField(TEXT("replicates"), bReplicates);
    }

    bool bReplicateMovement = false;
    if (Params->TryGetBoolField(TEXT("replicate_movement"), bReplicateMovement))
    {
        CDO->SetReplicateMovement(bReplicateMovement);
        bChanged = true;
        Applied->SetBoolField(TEXT("replicate_movement"), bReplicateMovement);
    }

    bool bAlwaysRelevant = false;
    if (Params->TryGetBoolField(TEXT("always_relevant"), bAlwaysRelevant))
    {
        CDO->bAlwaysRelevant = bAlwaysRelevant;
        bChanged = true;
        Applied->SetBoolField(TEXT("always_relevant"), bAlwaysRelevant);
    }

    double NetCull = 0.0;
    if (Params->TryGetNumberField(TEXT("net_cull_distance_squared"), NetCull))
    {
        // Public NetCullDistanceSquared access was deprecated in 5.5 — use the setter.
        CDO->SetNetCullDistanceSquared(static_cast<float>(NetCull));
        bChanged = true;
        Applied->SetNumberField(TEXT("net_cull_distance_squared"), NetCull);
    }

    if (!bChanged)
    {
        FPropertyChangedEvent NoEvt(nullptr, EPropertyChangeType::ValueSet);
        CDO->PostEditChangeProperty(NoEvt);
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("No replication fields provided"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Provide at least one of `replicates`, `replicate_movement`, `always_relevant`, `net_cull_distance_squared`."));
    }

    FPropertyChangedEvent ChangeEvent(nullptr, EPropertyChangeType::ValueSet);
    CDO->PostEditChangeProperty(ChangeEvent);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    Blueprint->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false))
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Replication flags set in memory but failed to save: %s"), *Blueprint->GetPathName()),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false (commonly blocked during PIE). Stop PIE and retry."));

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
    ResultObj->SetObjectField(TEXT("applied"), Applied);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleSetEventReplication(const TSharedPtr<FJsonObject>& Params)
{
    // Mirrors the editor's Details-panel "Replicates" dropdown + "Reliable" checkbox for a
    // Blueprint custom event. The editor path lives in
    // Engine/Source/Editor/Kismet/Private/BlueprintDetailsCustomization.cpp:
    //   - FBlueprintGraphActionDetails::SetNetFlags (line ~5118): for a UK2Node_CustomEvent it does
    //       FunctionFlags &= ~(FUNC_Net|FUNC_NetMulticast|FUNC_NetServer|FUNC_NetClient);
    //       FunctionFlags |= (NetFlags ? FUNC_Net|NetFlags : 0);
    //     then FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified (no ReconstructNode).
    //   - OnIsReliableReplicationFunctionModified (line ~6240): FunctionFlags |= / &= ~FUNC_NetReliable.
    // FUNC_* values are defined in Engine/Source/Runtime/CoreUObject/Public/UObject/Script.h
    // (FUNC_Net=0x40, FUNC_NetReliable=0x80, FUNC_NetMulticast=0x4000,
    //  FUNC_NetServer=0x200000, FUNC_NetClient=0x1000000). Dropdown→flag mapping is from
    // ReplicationSpecifierProperName (line ~6068): Multicast→FUNC_NetMulticast,
    // Run on Server→FUNC_NetServer, Run on owning Client→FUNC_NetClient, Not Replicated→0.
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
        return FMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name'"),
            EMCPErrorCode::InvalidArgument, TEXT("`blueprint_name` is required (short name or full asset path)."));

    FString EventName;
    if (!Params->TryGetStringField(TEXT("event_name"), EventName) || EventName.IsEmpty())
        return FMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'event_name'"),
            EMCPErrorCode::InvalidArgument, TEXT("`event_name` is required — the custom event's name (UK2Node_CustomEvent CustomFunctionName)."));

    FString Replication;
    if (!Params->TryGetStringField(TEXT("replication"), Replication))
        return FMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'replication'"),
            EMCPErrorCode::InvalidArgument, TEXT("`replication` is required: one of none | multicast | server | client."));

    // Map the dropdown value → the single net specifier bit (none == 0).
    uint32 NetSpecifier = 0;
    if (Replication.Equals(TEXT("none"), ESearchCase::IgnoreCase)) NetSpecifier = 0;
    else if (Replication.Equals(TEXT("multicast"), ESearchCase::IgnoreCase)) NetSpecifier = FUNC_NetMulticast;
    else if (Replication.Equals(TEXT("server"), ESearchCase::IgnoreCase)) NetSpecifier = FUNC_NetServer;
    else if (Replication.Equals(TEXT("client"), ESearchCase::IgnoreCase)) NetSpecifier = FUNC_NetClient;
    else
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Invalid 'replication' value: %s"), *Replication),
            EMCPErrorCode::InvalidArgument, TEXT("`replication` must be one of: none | multicast | server | client."));

    bool bReliable = false;
    Params->TryGetBoolField(TEXT("reliable"), bReliable);

    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
        return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
            EMCPErrorCode::AssetNotFound, TEXT("`blueprint_name` did not resolve. Use asset_list asset_type='Blueprint'."));

    // Find the custom event by name in the Ubergraph(s). A custom event is a UK2Node_CustomEvent
    // whose CustomFunctionName matches; a plain UK2Node_Event (override like BeginPlay) is NOT
    // settable here (no FunctionFlags member), so report that distinctly.
    UK2Node_CustomEvent* TargetEvent = nullptr;
    bool bFoundNonCustomMatch = false;
    const FName WantName(*EventName);
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
            {
                if (CE->CustomFunctionName == WantName)
                {
                    TargetEvent = CE;
                    break;
                }
            }
            else if (UK2Node_Event* EV = Cast<UK2Node_Event>(Node))
            {
                if (EV->EventReference.GetMemberName() == WantName)
                {
                    bFoundNonCustomMatch = true;
                }
            }
        }
        if (TargetEvent) break;
    }

    if (!TargetEvent)
    {
        if (bFoundNonCustomMatch)
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("'%s' is an engine/override event, not a custom event"), *EventName),
                EMCPErrorCode::UnsupportedClass,
                TEXT("Net specifiers can only be set on a Blueprint custom event (UK2Node_CustomEvent). Engine override events (BeginPlay/Tick/etc.) inherit their flags from C++."));
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Custom event not found: %s"), *EventName),
            EMCPErrorCode::NodeNotFound,
            TEXT("No UK2Node_CustomEvent with that name in the Event Graph. Names are case-insensitive. Use bp_read to enumerate event nodes."));
    }

    // Apply, mirroring SetNetFlags + the Reliable checkbox. Clear ALL net bits (including Reliable),
    // then OR the chosen specifier (with FUNC_Net) and Reliable (only meaningful when replicated).
    const int32 FlagsToClear = FUNC_Net | FUNC_NetMulticast | FUNC_NetServer | FUNC_NetClient | FUNC_NetReliable;
    int32 FlagsToSet = (NetSpecifier != 0) ? (FUNC_Net | static_cast<int32>(NetSpecifier)) : 0;
    if (NetSpecifier != 0 && bReliable)
    {
        FlagsToSet |= FUNC_NetReliable;
    }

    TargetEvent->Modify();
    TargetEvent->FunctionFlags &= ~FlagsToClear;
    TargetEvent->FunctionFlags |= FlagsToSet;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Blueprint->MarkPackageDirty();

    if (!UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false))
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Event net flags set in memory but failed to save: %s"), *Blueprint->GetPathName()),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false (commonly blocked during PIE). Stop PIE and retry."));

    const bool bEffectiveReliable = (NetSpecifier != 0) && bReliable;
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
    ResultObj->SetStringField(TEXT("event_name"), EventName);
    ResultObj->SetStringField(TEXT("replication"), Replication.ToLower());
    ResultObj->SetBoolField(TEXT("reliable"), bEffectiveReliable);
    ResultObj->SetNumberField(TEXT("function_flags"), static_cast<double>(TargetEvent->FunctionFlags));
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleSetMeshMaterialColor(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required (string). Accepts either a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints."));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'component_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`component_name` is required (string, the component's UObject name on the Blueprint's SCS — e.g. `StaticMesh1`, `DefaultSceneRoot`). Use `read_blueprint_content` to enumerate the Blueprint's SCS nodes."));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_name` did not resolve to a Blueprint via the usual lookup (short name against `/Game/Blueprints/`, then full asset path). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints. Names are case-sensitive."));
    }

    // Guard the SimpleConstructionScript before iterating it. A Blueprint whose
    // SCS is null (data-only / interface / function-library / anim Blueprints) would
    // null-deref on ->GetAllNodes() below → EXCEPTION_ACCESS_VIOLATION. Refuse cleanly.
    if (!Blueprint->SimpleConstructionScript)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint '%s' has no SimpleConstructionScript — it has no components"), *BlueprintName),
            EMCPErrorCode::UnsupportedClass,
            TEXT("This Blueprint has a null SimpleConstructionScript (it is not an Actor/Component-hosting Blueprint, e.g. a data-only or function-library Blueprint). Use `read_blueprint_content` to inspect; verify the Blueprint is an Actor subclass."));
    }

    // Find the component
    USCS_Node* ComponentNode = nullptr;
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node && Node->GetVariableName().ToString() == ComponentName)
        {
            ComponentNode = Node;
            break;
        }
    }

    if (!ComponentNode)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Component not found: %s"), *ComponentName),
            EMCPErrorCode::NodeNotFound,
            TEXT("`component_name` did not match any SCS node on the Blueprint. Component names match the SCS variable name (case-sensitive), not the display label. Use `read_blueprint_content` to enumerate the Blueprint's components."));
    }

    // Try to cast to StaticMeshComponent or PrimitiveComponent
    UPrimitiveComponent* PrimComponent = Cast<UPrimitiveComponent>(ComponentNode->ComponentTemplate);
    if (!PrimComponent)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Component is not a primitive component"),
            EMCPErrorCode::UnsupportedClass,
            TEXT("The resolved component is not a UPrimitiveComponent — this operation only works on primitive components (StaticMesh, SkeletalMesh, BoxComponent, SphereComponent, etc., which have physics + collision). Scene-only components (SceneComponent, USceneCaptureComponent, etc.) don't support it."));
    }

    // ── GAP-009: refuse the dynamic-material-instance path on a BP component template ──
    // `PrimComponent` here is ALWAYS a Blueprint SCS *component template*
    // (ComponentNode->ComponentTemplate) — an object serialized into the Blueprint
    // package and copied into every level that places the Blueprint. The old
    // implementation baked a runtime UMaterialInstanceDynamic (MID) into the template's
    // OverrideMaterials. A MID is transient/private/runtime-only (UMaterialInstanceDynamic::
    // Create outers it to the component and gives it no public/standalone flags), so
    // persisting it into a saved asset produces a cross-package reference to a private
    // object: any level placing this Blueprint then fails SaveMap with
    // "Illegal reference to private object: MaterialInstanceEditorOnlyData…". The MID was
    // correct for transient PLACED-INSTANCE coloring, but this handler can only ever reach
    // a template. Refuse and direct the caller to the SAVED-asset path, which round-trips
    // cleanly through level saves. (See docs/BUGS.md GAP-009.)
    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(
            TEXT("Refusing to bake a dynamic material instance into Blueprint component template '%s.%s': a UMaterialInstanceDynamic is runtime-only and would be persisted into the asset, breaking SaveMap for any level that places this Blueprint (GAP-009)."),
            *BlueprintName, *ComponentName),
        EMCPErrorCode::FeatureDisabled,
        TEXT("Color a Blueprint component's material through a SAVED Material Instance Constant, not a dynamic one: "
             "(1) material_create_instance(asset_path='/Game/.../MI_<Name>', parent_material=<the slot's base material, e.g. '/Engine/BasicShapes/BasicShapeMaterial'>); "
             "(2) material_instance_set_parameter(instance_path='/Game/.../MI_<Name>', parameter_name='BaseColor', parameter_type='vector', r, g, b, a) — repeat with parameter_name='Color' if the material exposes it; "
             "(3) material_apply_to_blueprint(blueprint_name, component_name, material_path='/Game/.../MI_<Name>', material_slot). "
             "That assigns a PUBLIC asset reference the package can save. mesh_set_mesh_material_color's dynamic-instance path is disabled because it corrupts package/level saves."));
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleGetAvailableMaterials(const TSharedPtr<FJsonObject>& Params)
{
    // Get parameters - make search path completely dynamic
    FString SearchPath;
    if (!Params->TryGetStringField(TEXT("search_path"), SearchPath))
    {
        // Default to empty string to search everywhere
        SearchPath = TEXT("");
    }
    
    bool bIncludeEngineMaterials = true;
    if (Params->HasField(TEXT("include_engine_materials")))
    {
        bIncludeEngineMaterials = Params->GetBoolField(TEXT("include_engine_materials"));
    }

    // Get asset registry module
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    // Create filter for materials
    FARFilter Filter;
    Filter.ClassPaths.Add(UMaterialInterface::StaticClass()->GetClassPathName());
    Filter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
    Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
    Filter.ClassPaths.Add(UMaterialInstanceDynamic::StaticClass()->GetClassPathName());
    
    // Add search paths dynamically
    if (!SearchPath.IsEmpty())
    {
        // Ensure the path starts with /
        if (!SearchPath.StartsWith(TEXT("/")))
        {
            SearchPath = TEXT("/") + SearchPath;
        }
        // Ensure the path ends with / for proper directory search
        if (!SearchPath.EndsWith(TEXT("/")))
        {
            SearchPath += TEXT("/");
        }
        Filter.PackagePaths.Add(*SearchPath);
        UE_LOG(LogUnrealMCP, Log, TEXT("Searching for materials in: %s"), *SearchPath);
    }
    else
    {
        // Search in common game content locations
        Filter.PackagePaths.Add(TEXT("/Game/"));
        UE_LOG(LogUnrealMCP, Log, TEXT("Searching for materials in all game content"));
    }
    
    if (bIncludeEngineMaterials)
    {
        Filter.PackagePaths.Add(TEXT("/Engine/"));
        UE_LOG(LogUnrealMCP, Log, TEXT("Including Engine materials in search"));
    }
    
    Filter.bRecursivePaths = true;

    // Get assets from registry
    TArray<FAssetData> AssetDataArray;
    AssetRegistry.GetAssets(Filter, AssetDataArray);
    
    UE_LOG(LogUnrealMCP, Log, TEXT("Asset registry found %d materials"), AssetDataArray.Num());

    // Also try manual search using EditorAssetLibrary for more comprehensive results
    TArray<FString> AllAssetPaths;
    if (!SearchPath.IsEmpty())
    {
        AllAssetPaths = UEditorAssetLibrary::ListAssets(SearchPath, true, false);
    }
    else
    {
        AllAssetPaths = UEditorAssetLibrary::ListAssets(TEXT("/Game/"), true, false);
    }
    
    // Filter for materials from the manual search
    for (const FString& AssetPath : AllAssetPaths)
    {
        if (AssetPath.Contains(TEXT("Material")) && !AssetPath.Contains(TEXT(".uasset")))
        {
            UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
            if (Asset && Asset->IsA<UMaterialInterface>())
            {
                // Check if we already have this asset from registry search
                bool bAlreadyFound = false;
                for (const FAssetData& ExistingData : AssetDataArray)
                {
                    if (ExistingData.GetObjectPathString() == AssetPath)
                    {
                        bAlreadyFound = true;
                        break;
                    }
                }
                
                if (!bAlreadyFound)
                {
                    // Create FAssetData manually for this asset
                    FAssetData ManualAssetData(Asset);
                    AssetDataArray.Add(ManualAssetData);
                }
            }
        }
    }

    UE_LOG(LogUnrealMCP, Log, TEXT("Total materials found after manual search: %d"), AssetDataArray.Num());

    // Convert to JSON
    TArray<TSharedPtr<FJsonValue>> MaterialArray;
    for (const FAssetData& AssetData : AssetDataArray)
    {
        TSharedPtr<FJsonObject> MaterialObj = MakeShared<FJsonObject>();
        MaterialObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
        MaterialObj->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
        MaterialObj->SetStringField(TEXT("package"), AssetData.PackageName.ToString());
        MaterialObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
        
        MaterialArray.Add(MakeShared<FJsonValueObject>(MaterialObj));
        
        UE_LOG(LogUnrealMCP, Verbose, TEXT("Found material: %s at %s"), *AssetData.AssetName.ToString(), *AssetData.GetObjectPathString());
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("materials"), MaterialArray);
    ResultObj->SetNumberField(TEXT("count"), MaterialArray.Num());
    ResultObj->SetStringField(TEXT("search_path_used"), SearchPath.IsEmpty() ? TEXT("/Game/") : SearchPath);
    
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleApplyMaterialToActor(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'actor_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`actor_name` is required (string, the actor's UObject name or ActorLabel). Use `get_actors_in_level` or `find_actors_by_name` to discover."));
    }

    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'material_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`material_path` is required (string, full `/Game/...` asset path to a UMaterial or UMaterialInstance). Use `list_assets` with `asset_type='Material'` / `'MaterialInstanceConstant'` to discover."));
    }

    int32 MaterialSlot = 0;
    if (Params->HasField(TEXT("material_slot")))
    {
        MaterialSlot = Params->GetIntegerField(TEXT("material_slot"));
    }

    // Find the actor
    AActor* TargetActor = nullptr;
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Failed to get editor world"),
            EMCPErrorCode::Internal,
            TEXT("GEditor->GetEditorWorldContext().World() returned null. The editor's world context is not yet initialized — typically only happens at editor startup or shutdown. Retry once the editor is fully loaded."));
    }
    
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }

    if (!TargetActor)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Actor not found: %s"), *ActorName),
            EMCPErrorCode::ActorNotFound,
            TEXT("No actor in the editor world matches `actor_name`. Names are case-sensitive and match the UObject name (or ActorLabel). Use `get_actors_in_level` or `find_actors_by_name` to discover."));
    }

    // Load the material
    UMaterialInterface* Material = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(MaterialPath));
    if (!Material)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to load material: %s"), *MaterialPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`material_path` did not resolve to a UMaterialInterface (UMaterial or UMaterialInstance) via LoadObject. Verify the path with `list_assets` (asset_type='Material' / 'MaterialInstanceConstant'). Paths are case-sensitive."));
    }

    // Optional per-component targeting (GAP-058). When `component_name` is supplied we apply the
    // material to ONLY the named static-mesh component; without it the historical behavior — apply
    // to every static-mesh component at the slot — is preserved (correct for single-mesh actors).
    // The old handler ignored `component_name` entirely, so targeting one slot on a multi-mesh actor
    // silently stomped ALL of them (a confident-but-wrong, data-losing edit).
    FString ComponentName;
    const bool bHasComponent = Params->TryGetStringField(TEXT("component_name"), ComponentName) && !ComponentName.IsEmpty();

    // Find mesh components and apply material
    TArray<UStaticMeshComponent*> MeshComponents;
    TargetActor->GetComponents<UStaticMeshComponent>(MeshComponents);

    TArray<TSharedPtr<FJsonValue>> AffectedComponents;
    for (UStaticMeshComponent* MeshComp : MeshComponents)
    {
        if (!MeshComp)
        {
            continue;
        }
        if (bHasComponent && MeshComp->GetName() != ComponentName)
        {
            continue;   // targeting a single component — skip the others
        }
        MeshComp->SetMaterial(MaterialSlot, Material);
        AffectedComponents.Add(MakeShared<FJsonValueString>(MeshComp->GetName()));
    }

    if (AffectedComponents.Num() == 0)
    {
        if (bHasComponent)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Component not found on actor: %s"), *ComponentName),
                EMCPErrorCode::NodeNotFound,
                TEXT("`component_name` did not match any UStaticMeshComponent on the resolved actor (matched against the component's UObject name, case-sensitive). Use `get_actor_material_info` or `actor_inspect` to enumerate the actor's component names; omit `component_name` to apply to every mesh component."));
        }
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("No mesh components found on actor"),
            EMCPErrorCode::InvalidArgument,
            TEXT("The resolved actor has no UMeshComponent (UStaticMeshComponent or USkeletalMeshComponent). This tool requires a mesh-bearing actor. Use `get_actor_material_info` to enumerate the actor's components, or pick a different actor."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("actor_name"), ActorName);
    ResultObj->SetStringField(TEXT("material_path"), MaterialPath);
    ResultObj->SetNumberField(TEXT("material_slot"), MaterialSlot);
    if (bHasComponent)
    {
        ResultObj->SetStringField(TEXT("component_name"), ComponentName);
    }
    // Echo exactly which components were painted so the caller can never mistake a broad apply for a
    // targeted one (the core of GAP-058 was a silent, over-broad edit reported as success).
    ResultObj->SetArrayField(TEXT("components_affected"), AffectedComponents);
    ResultObj->SetNumberField(TEXT("components_affected_count"), AffectedComponents.Num());
    ResultObj->SetBoolField(TEXT("success"), true);

    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleApplyMaterialToBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required (string). Accepts either a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints."));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'component_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`component_name` is required (string, the component's UObject name on the Blueprint's SCS — e.g. `StaticMesh1`, `DefaultSceneRoot`). Use `read_blueprint_content` to enumerate the Blueprint's SCS nodes."));
    }

    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'material_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`material_path` is required (string, full `/Game/...` asset path to a UMaterial or UMaterialInstance). Use `list_assets` with `asset_type='Material'` / `'MaterialInstanceConstant'` to discover."));
    }

    int32 MaterialSlot = 0;
    if (Params->HasField(TEXT("material_slot")))
    {
        MaterialSlot = Params->GetIntegerField(TEXT("material_slot"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_name` did not resolve to a Blueprint via the usual lookup (short name against `/Game/Blueprints/`, then full asset path). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints. Names are case-sensitive."));
    }

    // Guard the SimpleConstructionScript before iterating it. A Blueprint whose
    // SCS is null (data-only / interface / function-library / anim Blueprints) would
    // null-deref on ->GetAllNodes() below → EXCEPTION_ACCESS_VIOLATION. Refuse cleanly.
    if (!Blueprint->SimpleConstructionScript)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint '%s' has no SimpleConstructionScript — it has no components"), *BlueprintName),
            EMCPErrorCode::UnsupportedClass,
            TEXT("This Blueprint has a null SimpleConstructionScript (it is not an Actor/Component-hosting Blueprint, e.g. a data-only or function-library Blueprint). Use `read_blueprint_content` to inspect; verify the Blueprint is an Actor subclass."));
    }

    // Find the component
    USCS_Node* ComponentNode = nullptr;
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node && Node->GetVariableName().ToString() == ComponentName)
        {
            ComponentNode = Node;
            break;
        }
    }

    if (!ComponentNode)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Component not found: %s"), *ComponentName),
            EMCPErrorCode::NodeNotFound,
            TEXT("`component_name` did not match any SCS node on the Blueprint. Component names match the SCS variable name (case-sensitive), not the display label. Use `read_blueprint_content` to enumerate the Blueprint's components."));
    }

    UPrimitiveComponent* PrimComponent = Cast<UPrimitiveComponent>(ComponentNode->ComponentTemplate);
    if (!PrimComponent)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Component is not a primitive component"),
            EMCPErrorCode::UnsupportedClass,
            TEXT("The resolved component is not a UPrimitiveComponent — this operation only works on primitive components (StaticMesh, SkeletalMesh, BoxComponent, SphereComponent, etc., which have physics + collision). Scene-only components (SceneComponent, USceneCaptureComponent, etc.) don't support it."));
    }

    // Load the material
    UMaterialInterface* Material = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(MaterialPath));
    if (!Material)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to load material: %s"), *MaterialPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`material_path` did not resolve to a UMaterialInterface (UMaterial or UMaterialInstance) via LoadObject. Verify the path with `list_assets` (asset_type='Material' / 'MaterialInstanceConstant'). Paths are case-sensitive."));
    }

    // Apply the material
    PrimComponent->SetMaterial(MaterialSlot, Material);

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), BlueprintName);
    ResultObj->SetStringField(TEXT("component_name"), ComponentName);
    ResultObj->SetStringField(TEXT("material_path"), MaterialPath);
    ResultObj->SetNumberField(TEXT("material_slot"), MaterialSlot);
    ResultObj->SetBoolField(TEXT("success"), true);

    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleSetStaticMeshMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString MeshPath;
    if (!Params->TryGetStringField(TEXT("mesh_path"), MeshPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'mesh_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`mesh_path` is required (string, full `/Game/...` asset path to a UStaticMesh). Use `list_assets` with `asset_type='StaticMesh'` to discover existing meshes."));
    }

    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'material_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`material_path` is required (string, full `/Game/...` asset path to a UMaterial or UMaterialInstance). Use `list_assets` with `asset_type='Material'` / `'MaterialInstanceConstant'` to discover."));
    }

    int32 SlotIndex = 0;
    if (Params->HasField(TEXT("slot_index")))
    {
        SlotIndex = Params->GetIntegerField(TEXT("slot_index"));
    }

    UStaticMesh* StaticMesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(MeshPath));
    if (!StaticMesh)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to load static mesh: %s"), *MeshPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`mesh_path` did not resolve to a UStaticMesh via LoadAsset. Verify with `list_assets` (asset_type='StaticMesh'). Paths are case-sensitive."));
    }

    UMaterialInterface* Material = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(MaterialPath));
    if (!Material)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to load material: %s"), *MaterialPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`material_path` did not resolve to a UMaterialInterface via LoadAsset. Verify with `list_assets` (asset_type='Material' / 'MaterialInstanceConstant'). Paths are case-sensitive."));
    }

    const int32 SlotCount = StaticMesh->GetStaticMaterials().Num();
    if (SlotIndex < 0 || SlotIndex >= SlotCount)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Invalid slot_index %d (mesh has %d material slots)"), SlotIndex, SlotCount),
            EMCPErrorCode::OutOfRange,
            TEXT("`slot_index` must be in [0, num_material_slots). Use `get_blueprint_material_info` or `get_actor_material_info` on the mesh-bearing object to enumerate valid slot indices."));
    }

    // Universal mutation contract — see docs/mcp/USAGE.md.
    StaticMesh->Modify();
    StaticMesh->PreEditChange(nullptr);
    StaticMesh->GetStaticMaterials()[SlotIndex].MaterialInterface = Material;
    StaticMesh->PostEditChange();
    StaticMesh->MarkPackageDirty();

    if (!UEditorAssetLibrary::SaveAsset(StaticMesh->GetPathName(), /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Static mesh material slot mutated in-memory but failed to persist to disk: %s"), *StaticMesh->GetPathName()),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not written. SaveAsset no-ops while PIE is active or when the package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the change exists in-memory but will be lost on editor restart."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("mesh_path"), MeshPath);
    ResultObj->SetNumberField(TEXT("slot_index"), SlotIndex);
    ResultObj->SetStringField(TEXT("material_path"), MaterialPath);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleGetActorMaterialInfo(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'actor_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`actor_name` is required (string, the actor's UObject name or ActorLabel). Use `get_actors_in_level` or `find_actors_by_name` to discover."));
    }

    // Find the actor
    AActor* TargetActor = nullptr;
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Failed to get editor world"),
            EMCPErrorCode::Internal,
            TEXT("GEditor->GetEditorWorldContext().World() returned null. The editor's world context is not yet initialized — typically only happens at editor startup or shutdown. Retry once the editor is fully loaded."));
    }
    
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }

    if (!TargetActor)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Actor not found: %s"), *ActorName),
            EMCPErrorCode::ActorNotFound,
            TEXT("No actor in the editor world matches `actor_name`. Names are case-sensitive and match the UObject name (or ActorLabel). Use `get_actors_in_level` or `find_actors_by_name` to discover."));
    }

    // Get mesh components and their materials
    TArray<UStaticMeshComponent*> MeshComponents;
    TargetActor->GetComponents<UStaticMeshComponent>(MeshComponents);
    
    TArray<TSharedPtr<FJsonValue>> MaterialSlots;
    
    for (UStaticMeshComponent* MeshComp : MeshComponents)
    {
        if (MeshComp)
        {
            for (int32 i = 0; i < MeshComp->GetNumMaterials(); i++)
            {
                TSharedPtr<FJsonObject> SlotInfo = MakeShared<FJsonObject>();
                SlotInfo->SetNumberField(TEXT("slot"), i);
                SlotInfo->SetStringField(TEXT("component"), MeshComp->GetName());
                
                UMaterialInterface* Material = MeshComp->GetMaterial(i);
                if (Material)
                {
                    SlotInfo->SetStringField(TEXT("material_name"), Material->GetName());
                    SlotInfo->SetStringField(TEXT("material_path"), Material->GetPathName());
                    SlotInfo->SetStringField(TEXT("material_class"), Material->GetClass()->GetName());
                }
                else
                {
                    SlotInfo->SetStringField(TEXT("material_name"), TEXT("None"));
                    SlotInfo->SetStringField(TEXT("material_path"), TEXT(""));
                    SlotInfo->SetStringField(TEXT("material_class"), TEXT(""));
                }
                
                MaterialSlots.Add(MakeShared<FJsonValueObject>(SlotInfo));
            }
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("actor_name"), ActorName);
    ResultObj->SetArrayField(TEXT("material_slots"), MaterialSlots);
    ResultObj->SetNumberField(TEXT("total_slots"), MaterialSlots.Num());
    
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleGetBlueprintMaterialInfo(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required (string). Accepts either a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints."));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'component_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`component_name` is required (string, the component's UObject name on the Blueprint's SCS — e.g. `StaticMesh1`, `DefaultSceneRoot`). Use `read_blueprint_content` to enumerate the Blueprint's SCS nodes."));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_name` did not resolve to a Blueprint via the usual lookup (short name against `/Game/Blueprints/`, then full asset path). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints. Names are case-sensitive."));
    }

    // Guard the SimpleConstructionScript before iterating it. A Blueprint whose
    // SCS is null (data-only / interface / function-library / anim Blueprints) would
    // null-deref on ->GetAllNodes() below → EXCEPTION_ACCESS_VIOLATION. Refuse cleanly.
    if (!Blueprint->SimpleConstructionScript)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint '%s' has no SimpleConstructionScript — it has no components"), *BlueprintName),
            EMCPErrorCode::UnsupportedClass,
            TEXT("This Blueprint has a null SimpleConstructionScript (it is not an Actor/Component-hosting Blueprint, e.g. a data-only or function-library Blueprint). Use `read_blueprint_content` to inspect; verify the Blueprint is an Actor subclass."));
    }

    // Find the component
    USCS_Node* ComponentNode = nullptr;
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node && Node->GetVariableName().ToString() == ComponentName)
        {
            ComponentNode = Node;
            break;
        }
    }

    if (!ComponentNode)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Component not found: %s"), *ComponentName),
            EMCPErrorCode::NodeNotFound,
            TEXT("`component_name` did not match any SCS node on the Blueprint. Component names match the SCS variable name (case-sensitive), not the display label. Use `read_blueprint_content` to enumerate the Blueprint's components."));
    }

    UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(ComponentNode->ComponentTemplate);
    if (!MeshComponent)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Component is not a static mesh component"),
            EMCPErrorCode::UnsupportedClass,
            TEXT("The resolved component is not a UStaticMeshComponent — this operation requires a static-mesh component to set the mesh / material. For SkeletalMeshComponents, use the skeletal-mesh-specific tools instead."));
    }

    // Get material slot information
    TArray<TSharedPtr<FJsonValue>> MaterialSlots;
    int32 NumMaterials = 0;
    
    // Check if we have a static mesh assigned
    UStaticMesh* StaticMesh = MeshComponent->GetStaticMesh();
    if (StaticMesh)
    {
        NumMaterials = StaticMesh->GetNumSections(0); // Get number of material slots for LOD 0
        
        for (int32 i = 0; i < NumMaterials; i++)
        {
            TSharedPtr<FJsonObject> SlotInfo = MakeShared<FJsonObject>();
            SlotInfo->SetNumberField(TEXT("slot"), i);
            SlotInfo->SetStringField(TEXT("component"), ComponentName);
            
            UMaterialInterface* Material = MeshComponent->GetMaterial(i);
            if (Material)
            {
                SlotInfo->SetStringField(TEXT("material_name"), Material->GetName());
                SlotInfo->SetStringField(TEXT("material_path"), Material->GetPathName());
                SlotInfo->SetStringField(TEXT("material_class"), Material->GetClass()->GetName());
            }
            else
            {
                SlotInfo->SetStringField(TEXT("material_name"), TEXT("None"));
                SlotInfo->SetStringField(TEXT("material_path"), TEXT(""));
                SlotInfo->SetStringField(TEXT("material_class"), TEXT(""));
            }
            
            MaterialSlots.Add(MakeShared<FJsonValueObject>(SlotInfo));
        }
    }
    else
    {
        // If no static mesh is assigned, we can't determine material slots
        UE_LOG(LogUnrealMCP, Warning, TEXT("No static mesh assigned to component %s in blueprint %s"), *ComponentName, *BlueprintName);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_name"), BlueprintName);
    ResultObj->SetStringField(TEXT("component_name"), ComponentName);
    ResultObj->SetArrayField(TEXT("material_slots"), MaterialSlots);
    ResultObj->SetNumberField(TEXT("total_slots"), MaterialSlots.Num());
    ResultObj->SetBoolField(TEXT("has_static_mesh"), StaticMesh != nullptr);
    
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleReadBlueprintContent(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_path` is required (string, full `/Game/...` asset path to a Blueprint). Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    // Get optional parameters
    bool bIncludeEventGraph = true;
    bool bIncludeFunctions = true;
    bool bIncludeVariables = true;
    bool bIncludeComponents = true;
    bool bIncludeInterfaces = true;
    // Diff-mode opt-ins (default off — payload size jumps ~2-3x when enabled).
    // include_variable_defaults: resolve each variable's actual CDO value (the BP
    //   NewVariables.DefaultValue string is empty for object refs / many structs).
    // include_component_properties: per-component override list — every property
    //   that differs from the component's archetype, with current and archetype
    //   values serialized. This is what lets you spot a duplicated BP that lost
    //   its SkeletalMesh / AnimClass / object-ref defaults.
    // include_inherited_components: also emit components from parent BP chain and
    //   native (C++) parent-class components (e.g. ACharacter's Mesh + Capsule).
    bool bIncludeVariableDefaults = false;
    bool bIncludeComponentProperties = false;
    bool bIncludeInheritedComponents = false;

    Params->TryGetBoolField(TEXT("include_event_graph"), bIncludeEventGraph);
    Params->TryGetBoolField(TEXT("include_functions"), bIncludeFunctions);
    Params->TryGetBoolField(TEXT("include_variables"), bIncludeVariables);
    Params->TryGetBoolField(TEXT("include_components"), bIncludeComponents);
    Params->TryGetBoolField(TEXT("include_interfaces"), bIncludeInterfaces);
    Params->TryGetBoolField(TEXT("include_variable_defaults"), bIncludeVariableDefaults);
    Params->TryGetBoolField(TEXT("include_component_properties"), bIncludeComponentProperties);
    Params->TryGetBoolField(TEXT("include_inherited_components"), bIncludeInheritedComponents);

    // Load the blueprint — use FindBlueprintByName which has LoadObject + AssetRegistry fallbacks
    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprintByName(BlueprintPath);
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to load blueprint: %s"), *BlueprintPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_path` did not resolve to a UBlueprint via LoadObject. Verify the path with `list_assets` (asset_type='Blueprint'). Paths are case-sensitive and must include `/Game/` prefix; the .AssetName suffix is auto-handled."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_path"), BlueprintPath);
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : TEXT("None"));

    // Include variables if requested
    if (bIncludeVariables)
    {
        // Resolve the CDO once for the variable-defaults pass.  NewVariables.DefaultValue
        // is the literal string the author typed in the BP defaults panel — empty for
        // object refs and most structs.  The CDO carries the values the runtime sees.
        UObject* GeneratedCDO = (bIncludeVariableDefaults && Blueprint->GeneratedClass)
            ? Blueprint->GeneratedClass->GetDefaultObject()
            : nullptr;

        TArray<TSharedPtr<FJsonValue>> VariableArray;
        for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
        {
            TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
            VarObj->SetStringField(TEXT("name"), Variable.VarName.ToString());
            VarObj->SetStringField(TEXT("type"), Variable.VarType.PinCategory.ToString());
            VarObj->SetStringField(TEXT("default_value"), Variable.DefaultValue);
            VarObj->SetBoolField(TEXT("is_editable"), (Variable.PropertyFlags & CPF_Edit) != 0);

            if (GeneratedCDO)
            {
                if (FProperty* Prop = Blueprint->GeneratedClass->FindPropertyByName(Variable.VarName))
                {
                    FString CDOValueStr;
                    const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(GeneratedCDO);
                    Prop->ExportTextItem_Direct(CDOValueStr, ValuePtr, nullptr, nullptr, PPF_None);
                    VarObj->SetStringField(TEXT("cdo_value"), CDOValueStr);
                    VarObj->SetStringField(TEXT("cpp_type"), Prop->GetCPPType());
                }
                else
                {
                    VarObj->SetStringField(TEXT("cdo_value"), TEXT("<property not found on generated class>"));
                }
            }
            VariableArray.Add(MakeShared<FJsonValueObject>(VarObj));
        }
        ResultObj->SetArrayField(TEXT("variables"), VariableArray);
    }

    // Include functions if requested
    if (bIncludeFunctions)
    {
        TArray<TSharedPtr<FJsonValue>> FunctionArray;
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph)
            {
                TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
                FuncObj->SetStringField(TEXT("name"), Graph->GetName());
                FuncObj->SetStringField(TEXT("graph_type"), TEXT("Function"));
                
                // Count nodes in function
                int32 NodeCount = Graph->Nodes.Num();
                FuncObj->SetNumberField(TEXT("node_count"), NodeCount);
                
                FunctionArray.Add(MakeShared<FJsonValueObject>(FuncObj));
            }
        }
        ResultObj->SetArrayField(TEXT("functions"), FunctionArray);
    }

    // Include event graph if requested
    if (bIncludeEventGraph)
    {
        TSharedPtr<FJsonObject> EventGraphObj = MakeShared<FJsonObject>();
        
        // Find the main event graph
        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (Graph && Graph->GetName() == TEXT("EventGraph"))
            {
                EventGraphObj->SetStringField(TEXT("name"), Graph->GetName());
                EventGraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
                
                // Get basic node information
                TArray<TSharedPtr<FJsonValue>> NodeArray;
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    if (Node)
                    {
                        TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
                        NodeObj->SetStringField(TEXT("name"), Node->GetName());
                        NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
                        NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
                        NodeArray.Add(MakeShared<FJsonValueObject>(NodeObj));
                    }
                }
                EventGraphObj->SetArrayField(TEXT("nodes"), NodeArray);
                break;
            }
        }
        
        ResultObj->SetObjectField(TEXT("event_graph"), EventGraphObj);
    }

    // Include components if requested
    if (bIncludeComponents)
    {
        // Helper: serialize properties on `Obj` that differ from its archetype.
        // For SCS templates the archetype is the parent class's same SCS node (or the
        // C++ class default of the component class), so this captures exactly the
        // overrides set on the BP — which is precisely what gets lost in a bad
        // duplicate.  Returns one JSON object per overridden property with name,
        // type, current value, and archetype value.
        auto SerializeOverrides = [](const UObject* Obj) -> TArray<TSharedPtr<FJsonValue>>
        {
            TArray<TSharedPtr<FJsonValue>> Out;
            if (!Obj) return Out;
            const UObject* Archetype = Obj->GetArchetype();
            if (!Archetype) return Out;

            for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
            {
                FProperty* Prop = *It;
                if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient
                    | CPF_NonPIEDuplicateTransient | CPF_Deprecated))
                {
                    continue;
                }
                const void* ObjValue = Prop->ContainerPtrToValuePtr<void>(Obj);
                const void* ArchValue = Prop->ContainerPtrToValuePtr<void>(Archetype);
                if (Prop->Identical(ObjValue, ArchValue, PPF_DeepComparison))
                {
                    continue;
                }

                TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
                PropObj->SetStringField(TEXT("name"), Prop->GetName());
                PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());

                FString CurStr;
                Prop->ExportTextItem_Direct(CurStr, ObjValue, nullptr, nullptr, PPF_None);
                PropObj->SetStringField(TEXT("value"), CurStr);

                FString ArchStr;
                Prop->ExportTextItem_Direct(ArchStr, ArchValue, nullptr, nullptr, PPF_None);
                PropObj->SetStringField(TEXT("archetype_value"), ArchStr);

                Out.Add(MakeShared<FJsonValueObject>(PropObj));
            }
            return Out;
        };

        // Helper: emit one component as a JSON object.  Used for SCS, inherited-SCS,
        // and native components — the per-source flags (inherited / native / declared_in)
        // are filled in by the caller.
        auto EmitComponent = [&](const UActorComponent* CompTemplate,
                                 const FString& VarName,
                                 const FString& ParentName,
                                 const FName& AttachSocket,
                                 bool bIsRoot,
                                 bool bInherited,
                                 bool bNative,
                                 const FString& DeclaredIn) -> TSharedPtr<FJsonObject>
        {
            TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
            CompObj->SetStringField(TEXT("name"), VarName);
            CompObj->SetStringField(TEXT("class"),
                CompTemplate ? CompTemplate->GetClass()->GetName() : FString(TEXT("Unknown")));
            CompObj->SetBoolField(TEXT("is_root"), bIsRoot);
            if (!ParentName.IsEmpty())
            {
                CompObj->SetStringField(TEXT("parent_component"), ParentName);
            }
            if (!AttachSocket.IsNone())
            {
                CompObj->SetStringField(TEXT("attach_socket"), AttachSocket.ToString());
            }
            if (bInherited)
            {
                CompObj->SetBoolField(TEXT("inherited"), true);
            }
            if (bNative)
            {
                CompObj->SetBoolField(TEXT("native"), true);
            }
            if (!DeclaredIn.IsEmpty())
            {
                CompObj->SetStringField(TEXT("declared_in"), DeclaredIn);
            }
            if (bIncludeComponentProperties && CompTemplate)
            {
                CompObj->SetArrayField(TEXT("property_overrides"),
                    SerializeOverrides(CompTemplate));
            }
            return CompObj;
        };

        TArray<TSharedPtr<FJsonValue>> ComponentArray;
        TSet<FName> EmittedNames; // dedupe across SCS chain + native CDO sweep

        // (1) This BP's own SCS — the components the BP author added directly.
        if (Blueprint->SimpleConstructionScript)
        {
            TMap<USCS_Node*, USCS_Node*> ParentMap;
            for (USCS_Node* SCSNode : Blueprint->SimpleConstructionScript->GetAllNodes())
            {
                if (SCSNode)
                {
                    for (USCS_Node* Child : SCSNode->ChildNodes)
                    {
                        ParentMap.Add(Child, SCSNode);
                    }
                }
            }

            for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
            {
                if (Node && Node->ComponentTemplate)
                {
                    const FName VarName = Node->GetVariableName();
                    EmittedNames.Add(VarName);
                    FString ParentName;
                    if (USCS_Node** ParentPtr = ParentMap.Find(Node))
                    {
                        ParentName = (*ParentPtr)->GetVariableName().ToString();
                    }
                    ComponentArray.Add(MakeShared<FJsonValueObject>(EmitComponent(
                        Node->ComponentTemplate,
                        VarName.ToString(),
                        ParentName,
                        Node->AttachToName,
                        Node == Blueprint->SimpleConstructionScript->GetDefaultSceneRootNode(),
                        /*bInherited=*/false,
                        /*bNative=*/false,
                        /*DeclaredIn=*/FString())));
                }
            }
        }

        // (2) Inherited components — walk the parent BPGC chain (BP-added components
        //     on parent BPs) plus native (C++) components on the parent CDO chain.
        //     This is where ACharacter::Mesh and ::CapsuleComponent show up — and
        //     where SkeletalMesh / AnimClass property overrides live for FPS/horse BPs.
        if (bIncludeInheritedComponents && Blueprint->GeneratedClass)
        {
            // (2a) Walk parent BP chain SCS — inherited BP-added components.
            UClass* CurrentClass = Blueprint->GeneratedClass->GetSuperClass();
            while (CurrentClass)
            {
                UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(CurrentClass);
                if (BPGC && BPGC->SimpleConstructionScript)
                {
                    for (USCS_Node* Node : BPGC->SimpleConstructionScript->GetAllNodes())
                    {
                        if (Node && Node->ComponentTemplate)
                        {
                            const FName VarName = Node->GetVariableName();
                            if (EmittedNames.Contains(VarName)) continue;
                            EmittedNames.Add(VarName);
                            ComponentArray.Add(MakeShared<FJsonValueObject>(EmitComponent(
                                Node->ComponentTemplate,
                                VarName.ToString(),
                                /*ParentName=*/FString(),
                                Node->AttachToName,
                                /*bIsRoot=*/false,
                                /*bInherited=*/true,
                                /*bNative=*/false,
                                /*DeclaredIn=*/CurrentClass->GetName())));
                        }
                    }
                }
                CurrentClass = CurrentClass->GetSuperClass();
            }

            // (2b) Native components on the CDO — anything CreateDefaultSubobject'd
            //      in a C++ constructor (ACharacter's Mesh/CapsuleComponent, custom
            //      C++ components on AHorseBase, etc.).
            if (AActor* ActorCDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject()))
            {
                TInlineComponentArray<UActorComponent*> NativeComps;
                ActorCDO->GetComponents(NativeComps);
                for (UActorComponent* Comp : NativeComps)
                {
                    if (!Comp) continue;
                    if (Comp->CreationMethod != EComponentCreationMethod::Native) continue;
                    const FName VarName = Comp->GetFName();
                    if (EmittedNames.Contains(VarName)) continue;
                    EmittedNames.Add(VarName);

                    FString ParentName;
                    FName AttachSocket = NAME_None;
                    if (USceneComponent* SceneComp = Cast<USceneComponent>(Comp))
                    {
                        if (USceneComponent* AttachParent = SceneComp->GetAttachParent())
                        {
                            ParentName = AttachParent->GetFName().ToString();
                        }
                        AttachSocket = SceneComp->GetAttachSocketName();
                    }

                    // declared_in: walk back up to find the C++ class that first
                    // declared this property (best-effort via the property name).
                    FString DeclaredIn;
                    if (FProperty* Prop = ActorCDO->GetClass()->FindPropertyByName(VarName))
                    {
                        if (UStruct* Owner = Prop->GetOwnerStruct())
                        {
                            DeclaredIn = Owner->GetName();
                        }
                    }

                    ComponentArray.Add(MakeShared<FJsonValueObject>(EmitComponent(
                        Comp,
                        VarName.ToString(),
                        ParentName,
                        AttachSocket,
                        /*bIsRoot=*/Comp == ActorCDO->GetRootComponent(),
                        /*bInherited=*/true,
                        /*bNative=*/true,
                        DeclaredIn)));
                }
            }
        }

        ResultObj->SetArrayField(TEXT("components"), ComponentArray);
    }

    // Include interfaces if requested
    if (bIncludeInterfaces)
    {
        TArray<TSharedPtr<FJsonValue>> InterfaceArray;
        for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
        {
            TSharedPtr<FJsonObject> InterfaceObj = MakeShared<FJsonObject>();
            InterfaceObj->SetStringField(TEXT("name"), Interface.Interface ? Interface.Interface->GetName() : TEXT("Unknown"));
            InterfaceArray.Add(MakeShared<FJsonValueObject>(InterfaceObj));
        }
        ResultObj->SetArrayField(TEXT("interfaces"), InterfaceArray);
    }

    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleAnalyzeBlueprintGraph(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_path` is required (string, full `/Game/...` asset path to a Blueprint). Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString GraphName = TEXT("EventGraph");
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    // Transition disambiguation — when multiple transitions share the name "Transition",
    // from_state/to_state uniquely identifies the one between two specific states.
    FString TransFromState, TransToState;
    Params->TryGetStringField(TEXT("from_state"), TransFromState);
    Params->TryGetStringField(TEXT("to_state"), TransToState);

    // Get optional parameters
    bool bIncludeNodeDetails = true;
    bool bIncludePinConnections = true;
    bool bTraceExecutionFlow = true;

    Params->TryGetBoolField(TEXT("include_node_details"), bIncludeNodeDetails);
    Params->TryGetBoolField(TEXT("include_pin_connections"), bIncludePinConnections);
    Params->TryGetBoolField(TEXT("trace_execution_flow"), bTraceExecutionFlow);

    // GAP-032: dense graphs can overflow the tool token cap (~91k chars seen).
    // Support a summary mode and pagination so callers can bound the payload:
    //   detail="summary" → name/class/title only (forces details + pins off)
    //   max_nodes        → cap nodes emitted this call (0 = no cap)
    //   offset           → skip the first N nodes (pagination cursor)
    FString DetailLevel = TEXT("full");
    Params->TryGetStringField(TEXT("detail"), DetailLevel);
    if (DetailLevel.Equals(TEXT("summary"), ESearchCase::IgnoreCase))
    {
        bIncludeNodeDetails = false;
        bIncludePinConnections = false;
    }
    int32 MaxNodes = 0;
    { double D = 0.0; if (Params->TryGetNumberField(TEXT("max_nodes"), D)) MaxNodes = static_cast<int32>(D); }
    int32 NodeOffset = 0;
    { double D = 0.0; if (Params->TryGetNumberField(TEXT("offset"), D)) NodeOffset = FMath::Max(0, static_cast<int32>(D)); }

    // Load the blueprint — use FindBlueprintByName which has LoadObject + AssetRegistry fallbacks
    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprintByName(BlueprintPath);
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to load blueprint: %s"), *BlueprintPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_path` did not resolve to a UBlueprint via LoadObject. Verify the path with `list_assets` (asset_type='Blueprint'). Paths are case-sensitive and must include `/Game/` prefix; the .AssetName suffix is auto-handled."));
    }

    // Find the specified graph — FindGraphByName handles state machine sub-graphs
    // and uses from_state/to_state to disambiguate transitions with the same name.
    UEdGraph* TargetGraph = FMCPCommonUtils::FindGraphByName(
        Blueprint, GraphName, TransFromState, TransToState);

    if (!TargetGraph)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Graph not found: %s"), *GraphName),
            EMCPErrorCode::NodeNotFound,
            TEXT("`graph_name` did not match any UEdGraph on the Blueprint. Graph names match the UEdGraph's name (the event graph is typically `EventGraph`; function graphs are named for the function). Use `list_blueprint_graphs` to enumerate."));
    }

    TSharedPtr<FJsonObject> GraphData = MakeShared<FJsonObject>();
    GraphData->SetStringField(TEXT("graph_name"), TargetGraph->GetName());
    GraphData->SetStringField(TEXT("graph_type"), TargetGraph->GetClass()->GetName());

    GraphData->SetNumberField(TEXT("total_nodes"), TargetGraph->Nodes.Num());

    // Analyze nodes
    TArray<TSharedPtr<FJsonValue>> NodeArray;
    TArray<TSharedPtr<FJsonValue>> ConnectionArray;

    int32 NodeIdx = -1;
    for (UEdGraphNode* Node : TargetGraph->Nodes)
    {
        if (Node)
        {
            // GAP-032 pagination: count/skip/cap over non-null nodes.
            ++NodeIdx;
            if (NodeIdx < NodeOffset) { continue; }
            if (MaxNodes > 0 && NodeArray.Num() >= MaxNodes) { break; }

            TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
            NodeObj->SetStringField(TEXT("name"), Node->GetName());
            NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
            NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

            if (bIncludeNodeDetails)
            {
                NodeObj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
                NodeObj->SetNumberField(TEXT("pos_y"), Node->NodePosY);
                NodeObj->SetBoolField(TEXT("can_rename"), Node->bCanRenameNode);

                // Comment nodes: include bounding box for spatial containment queries
                if (UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(Node))
                {
                    NodeObj->SetNumberField(TEXT("width"), CommentNode->NodeWidth);
                    NodeObj->SetNumberField(TEXT("height"), CommentNode->NodeHeight);
                }

                // Node comment text (any node can have a comment)
                if (!Node->NodeComment.IsEmpty())
                {
                    NodeObj->SetStringField(TEXT("comment"), Node->NodeComment);
                }

                // K2Node_CallFunction (and subclass K2Node_Message): expose function reference metadata
                if (UK2Node_CallFunction* CallFuncNode = Cast<UK2Node_CallFunction>(Node))
                {
                    FName FuncName = CallFuncNode->FunctionReference.GetMemberName();
                    if (!FuncName.IsNone())
                    {
                        NodeObj->SetStringField(TEXT("function_name"), FuncName.ToString());
                    }
                    UClass* TargetClass = CallFuncNode->FunctionReference.GetMemberParentClass();
                    if (TargetClass)
                    {
                        NodeObj->SetStringField(TEXT("target_class"), TargetClass->GetName());
                    }
                    // Flag whether this is an interface message call (K2Node_Message)
                    if (Node->GetClass()->GetName() == TEXT("K2Node_Message"))
                    {
                        NodeObj->SetBoolField(TEXT("is_interface_message"), true);
                    }
                }

                // K2Node_DynamicCast: expose cast target class
                if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
                {
                    if (CastNode->TargetType)
                    {
                        NodeObj->SetStringField(TEXT("cast_target"), CastNode->TargetType->GetName());
                    }
                }

                // K2Node_Event: expose event reference metadata
                if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
                {
                    FName EventFuncName = EventNode->EventReference.GetMemberName();
                    if (!EventFuncName.IsNone())
                    {
                        NodeObj->SetStringField(TEXT("event_function"), EventFuncName.ToString());
                    }
                    UClass* EventClass = EventNode->EventReference.GetMemberParentClass();
                    if (EventClass)
                    {
                        NodeObj->SetStringField(TEXT("event_class"), EventClass->GetName());
                    }
                }

                // K2Node_ComponentBoundEvent: expose delegate + component binding
                if (UK2Node_ComponentBoundEvent* CBENode = Cast<UK2Node_ComponentBoundEvent>(Node))
                {
                    if (!CBENode->DelegatePropertyName.IsNone())
                    {
                        NodeObj->SetStringField(TEXT("delegate_name"), CBENode->DelegatePropertyName.ToString());
                    }
                    if (CBENode->DelegateOwnerClass)
                    {
                        NodeObj->SetStringField(TEXT("delegate_class"), CBENode->DelegateOwnerClass->GetName());
                    }
                    FName CompName = CBENode->GetComponentPropertyName();
                    if (!CompName.IsNone())
                    {
                        NodeObj->SetStringField(TEXT("component_name"), CompName.ToString());
                    }
                }

                // K2Node_CustomEvent: expose replication and call-in-editor flags
                if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
                {
                    if (CustomEventNode->bCallInEditor)
                    {
                        NodeObj->SetBoolField(TEXT("call_in_editor"), true);
                    }
                    if (CustomEventNode->bIsDeprecated)
                    {
                        NodeObj->SetBoolField(TEXT("is_deprecated"), true);
                    }
                    uint32 NetFlags = CustomEventNode->GetNetFlags();
                    if (NetFlags & FUNC_NetMulticast)
                    {
                        NodeObj->SetStringField(TEXT("replication"), TEXT("Multicast"));
                    }
                    else if (NetFlags & FUNC_NetServer)
                    {
                        NodeObj->SetStringField(TEXT("replication"), TEXT("RunOnServer"));
                    }
                    else if (NetFlags & FUNC_NetClient)
                    {
                        NodeObj->SetStringField(TEXT("replication"), TEXT("RunOnClient"));
                    }
                }

                // K2Node_Timeline: expose timeline name
                if (UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node))
                {
                    if (!TimelineNode->TimelineName.IsNone())
                    {
                        NodeObj->SetStringField(TEXT("timeline_name"), TimelineNode->TimelineName.ToString());
                    }
                }

                // K2Node_MacroInstance: expose source macro graph name
                if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
                {
                    UEdGraph* MacroGraph = MacroNode->GetMacroGraph();
                    if (MacroGraph)
                    {
                        NodeObj->SetStringField(TEXT("macro_graph"), MacroGraph->GetName());
                    }
                    UBlueprint* MacroBP = MacroNode->GetSourceBlueprint();
                    if (MacroBP)
                    {
                        NodeObj->SetStringField(TEXT("macro_source"), MacroBP->GetName());
                    }
                }

                // K2Node_EnhancedInputAction: expose input action name
                if (UK2Node_EnhancedInputAction* InputActionNode = Cast<UK2Node_EnhancedInputAction>(Node))
                {
                    if (InputActionNode->InputAction)
                    {
                        NodeObj->SetStringField(TEXT("input_action"), InputActionNode->InputAction->GetName());
                    }
                }

                // AnimGraphNode_ModifyBone: expose target bone
                if (UAnimGraphNode_ModifyBone* ModBoneNode = Cast<UAnimGraphNode_ModifyBone>(Node))
                {
                    if (!ModBoneNode->Node.BoneToModify.BoneName.IsNone())
                    {
                        NodeObj->SetStringField(TEXT("target_bone"), ModBoneNode->Node.BoneToModify.BoneName.ToString());
                    }
                }

                // AnimGraphNode_TwoBoneIK: expose IK bone, target bone, and effector space
                if (UAnimGraphNode_TwoBoneIK* TwoBoneIKNode = Cast<UAnimGraphNode_TwoBoneIK>(Node))
                {
                    if (!TwoBoneIKNode->Node.IKBone.BoneName.IsNone())
                    {
                        NodeObj->SetStringField(TEXT("ik_bone"), TwoBoneIKNode->Node.IKBone.BoneName.ToString());
                    }
                    NodeObj->SetStringField(TEXT("effector_space"),
                        StaticEnum<EBoneControlSpace>()->GetNameStringByValue(
                            static_cast<int64>(TwoBoneIKNode->Node.EffectorLocationSpace)));
                }

                // AnimGraphNode_SequencePlayer: animation asset, play rate, loop
                if (UAnimGraphNode_SequencePlayer* SeqPlayer = Cast<UAnimGraphNode_SequencePlayer>(Node))
                {
                    if (UAnimSequenceBase* Seq = SeqPlayer->Node.GetSequence())
                    {
                        NodeObj->SetStringField(TEXT("anim_sequence"), Seq->GetPathName());
                        NodeObj->SetStringField(TEXT("anim_sequence_name"), Seq->GetName());
                    }
                    NodeObj->SetNumberField(TEXT("play_rate"), SeqPlayer->Node.GetPlayRate());
                    NodeObj->SetBoolField(TEXT("loop"), SeqPlayer->Node.IsLooping());
                }

                // AnimGraphNode_BlendSpacePlayer: blend space asset
                if (UAnimGraphNode_BlendSpacePlayer* BSPlayer = Cast<UAnimGraphNode_BlendSpacePlayer>(Node))
                {
                    if (UBlendSpace* BS = BSPlayer->Node.GetBlendSpace())
                    {
                        NodeObj->SetStringField(TEXT("blend_space"), BS->GetPathName());
                        NodeObj->SetStringField(TEXT("blend_space_name"), BS->GetName());
                    }
                }

                // AnimGraphNode_Slot: slot name
                if (UAnimGraphNode_Slot* SlotNode = Cast<UAnimGraphNode_Slot>(Node))
                {
                    if (!SlotNode->Node.SlotName.IsNone())
                    {
                        NodeObj->SetStringField(TEXT("slot_name"), SlotNode->Node.SlotName.ToString());
                    }
                }

                // AnimGraphNode_SaveCachedPose: cache name
                if (UAnimGraphNode_SaveCachedPose* SaveNode = Cast<UAnimGraphNode_SaveCachedPose>(Node))
                {
                    if (!SaveNode->CacheName.IsEmpty())
                    {
                        NodeObj->SetStringField(TEXT("cache_name"), SaveNode->CacheName);
                    }
                }

                // AnimGraphNode_UseCachedPose: linked cache name
                if (UAnimGraphNode_UseCachedPose* UseNode = Cast<UAnimGraphNode_UseCachedPose>(Node))
                {
                    if (UseNode->SaveCachedPoseNode.IsValid())
                    {
                        NodeObj->SetStringField(TEXT("cache_name"), UseNode->SaveCachedPoseNode->CacheName);
                    }
                }

                // AnimGraphNode_StateMachine: machine name and state count
                if (UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node))
                {
                    if (SMNode->EditorStateMachineGraph)
                    {
                        NodeObj->SetStringField(TEXT("machine_name"), SMNode->EditorStateMachineGraph->GetName());
                        int32 StateCount = 0;
                        for (UEdGraphNode* SMChild : SMNode->EditorStateMachineGraph->Nodes)
                        {
                            if (Cast<UAnimStateNode>(SMChild)) StateCount++;
                        }
                        NodeObj->SetNumberField(TEXT("state_count"), StateCount);
                    }
                }

                // AnimGraphNode_LinkedAnimGraph: linked anim class
                if (UAnimGraphNode_LinkedAnimGraph* LinkedNode = Cast<UAnimGraphNode_LinkedAnimGraph>(Node))
                {
                    if (LinkedNode->Node.InstanceClass)
                    {
                        NodeObj->SetStringField(TEXT("linked_class"), LinkedNode->Node.InstanceClass->GetPathName());
                        NodeObj->SetStringField(TEXT("linked_class_name"), LinkedNode->Node.InstanceClass->GetName());
                    }
                }

                // AnimGraphNode_LayeredBoneBlend: layer count and branch filters
                if (UAnimGraphNode_LayeredBoneBlend* LBBNode = Cast<UAnimGraphNode_LayeredBoneBlend>(Node))
                {
                    NodeObj->SetNumberField(TEXT("layer_count"), LBBNode->Node.LayerSetup.Num());
                    NodeObj->SetStringField(TEXT("blend_mode"),
                        LBBNode->Node.BlendMode == ELayeredBoneBlendMode::BranchFilter ? TEXT("BranchFilter") : TEXT("BlendMask"));

                    // Serialize per-layer branch filters
                    TArray<TSharedPtr<FJsonValue>> LayersArray;
                    for (int32 LayerIdx = 0; LayerIdx < LBBNode->Node.LayerSetup.Num(); ++LayerIdx)
                    {
                        const FInputBlendPose& Layer = LBBNode->Node.LayerSetup[LayerIdx];
                        TSharedPtr<FJsonObject> LayerObj = MakeShared<FJsonObject>();
                        LayerObj->SetNumberField(TEXT("index"), LayerIdx);

                        TArray<TSharedPtr<FJsonValue>> FiltersArray;
                        for (const FBranchFilter& Filter : Layer.BranchFilters)
                        {
                            TSharedPtr<FJsonObject> FilterObj = MakeShared<FJsonObject>();
                            FilterObj->SetStringField(TEXT("bone_name"), Filter.BoneName.ToString());
                            FilterObj->SetNumberField(TEXT("blend_depth"), Filter.BlendDepth);
                            FiltersArray.Add(MakeShared<FJsonValueObject>(FilterObj));
                        }
                        LayerObj->SetArrayField(TEXT("branch_filters"), FiltersArray);
                        LayersArray.Add(MakeShared<FJsonValueObject>(LayerObj));
                    }
                    NodeObj->SetArrayField(TEXT("layers"), LayersArray);

                    // Serialize blend masks if in BlendMask mode
                    if (LBBNode->Node.BlendMode == ELayeredBoneBlendMode::BlendMask)
                    {
                        TArray<TSharedPtr<FJsonValue>> MasksArray;
                        for (int32 MaskIdx = 0; MaskIdx < LBBNode->Node.BlendMasks.Num(); ++MaskIdx)
                        {
                            TSharedPtr<FJsonObject> MaskObj = MakeShared<FJsonObject>();
                            MaskObj->SetNumberField(TEXT("index"), MaskIdx);
                            if (LBBNode->Node.BlendMasks[MaskIdx])
                            {
                                MaskObj->SetStringField(TEXT("blend_profile"), LBBNode->Node.BlendMasks[MaskIdx]->GetPathName());
                                MaskObj->SetStringField(TEXT("blend_profile_name"), LBBNode->Node.BlendMasks[MaskIdx]->GetName());
                            }
                            MasksArray.Add(MakeShared<FJsonValueObject>(MaskObj));
                        }
                        NodeObj->SetArrayField(TEXT("blend_masks"), MasksArray);
                    }
                }

                // AnimGraphNode_BlendListByBool
                if (UAnimGraphNode_BlendListByBool* BLBNode = Cast<UAnimGraphNode_BlendListByBool>(Node))
                {
                    NodeObj->SetStringField(TEXT("blend_type"), TEXT("BlendListByBool"));
                }

                // ── State machine inner node types ─────────────────────────

                // UAnimStateNode: a state in a state machine
                if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
                {
                    NodeObj->SetStringField(TEXT("state_type"), TEXT("state"));
                    NodeObj->SetStringField(TEXT("state_name"), StateNode->GetStateName());
                    if (StateNode->BoundGraph)
                    {
                        NodeObj->SetStringField(TEXT("inner_graph"), StateNode->BoundGraph->GetName());
                        NodeObj->SetNumberField(TEXT("inner_graph_node_count"), StateNode->BoundGraph->Nodes.Num());
                    }
                }

                // UAnimStateTransitionNode: transition between states
                if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(Node))
                {
                    NodeObj->SetStringField(TEXT("state_type"), TEXT("transition"));
                    if (TransNode->GetPreviousState())
                    {
                        NodeObj->SetStringField(TEXT("from_state"), TransNode->GetPreviousState()->GetStateName());
                    }
                    if (TransNode->GetNextState())
                    {
                        NodeObj->SetStringField(TEXT("to_state"), TransNode->GetNextState()->GetStateName());
                    }
                    if (TransNode->BoundGraph)
                    {
                        NodeObj->SetStringField(TEXT("rule_graph"), TransNode->BoundGraph->GetName());
                    }
                    NodeObj->SetNumberField(TEXT("priority_order"), TransNode->PriorityOrder);
                    NodeObj->SetBoolField(TEXT("bidirectional"), TransNode->Bidirectional);
                }

                // UAnimStateEntryNode: start state marker
                if (UAnimStateEntryNode* EntryNode = Cast<UAnimStateEntryNode>(Node))
                {
                    NodeObj->SetStringField(TEXT("state_type"), TEXT("entry"));
                }
            }

            // Include pin information if requested
            if (bIncludePinConnections)
            {
                TArray<TSharedPtr<FJsonValue>> PinArray;
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (Pin)
                    {
                        TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
                        PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
                        PinObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
                        PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
                        PinObj->SetNumberField(TEXT("connections"), Pin->LinkedTo.Num());

                        // Pin default value — inline constants, enum selections, etc.
                        if (!Pin->DefaultValue.IsEmpty())
                        {
                            PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
                        }
                        // Some pins use DefaultTextValue or DefaultObject instead
                        if (!Pin->DefaultTextValue.IsEmpty())
                        {
                            PinObj->SetStringField(TEXT("default_text_value"), Pin->DefaultTextValue.ToString());
                        }
                        if (Pin->DefaultObject)
                        {
                            PinObj->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
                        }
                        // Sub-category gives more type detail (e.g., struct name for "struct" pins)
                        if (!Pin->PinType.PinSubCategory.IsNone())
                        {
                            PinObj->SetStringField(TEXT("sub_category"), Pin->PinType.PinSubCategory.ToString());
                        }
                        if (Pin->PinType.PinSubCategoryObject.IsValid())
                        {
                            PinObj->SetStringField(TEXT("sub_category_object"), Pin->PinType.PinSubCategoryObject->GetName());
                        }
                        
                        // Record connections for this pin
                        for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                        {
                            if (LinkedPin && LinkedPin->GetOwningNode())
                            {
                                TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
                                ConnObj->SetStringField(TEXT("from_node"), Pin->GetOwningNode()->GetName());
                                ConnObj->SetStringField(TEXT("from_pin"), Pin->PinName.ToString());
                                ConnObj->SetStringField(TEXT("to_node"), LinkedPin->GetOwningNode()->GetName());
                                ConnObj->SetStringField(TEXT("to_pin"), LinkedPin->PinName.ToString());
                                ConnectionArray.Add(MakeShared<FJsonValueObject>(ConnObj));
                            }
                        }
                        
                        PinArray.Add(MakeShared<FJsonValueObject>(PinObj));
                    }
                }
                NodeObj->SetArrayField(TEXT("pins"), PinArray);
            }

            NodeArray.Add(MakeShared<FJsonValueObject>(NodeObj));
        }
    }

    GraphData->SetArrayField(TEXT("nodes"), NodeArray);
    GraphData->SetArrayField(TEXT("connections"), ConnectionArray);
    GraphData->SetNumberField(TEXT("returned_nodes"), NodeArray.Num());
    // Emit a pagination cursor when this call capped the node window.
    if (MaxNodes > 0 && (NodeOffset + NodeArray.Num()) < TargetGraph->Nodes.Num())
    {
        GraphData->SetNumberField(TEXT("next_offset"), NodeOffset + NodeArray.Num());
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_path"), BlueprintPath);
    ResultObj->SetObjectField(TEXT("graph_data"), GraphData);
    ResultObj->SetBoolField(TEXT("success"), true);

    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleGetBlueprintVariableDetails(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_path` is required (string, full `/Game/...` asset path to a Blueprint). Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString VariableName;
    bool bSpecificVariable = Params->TryGetStringField(TEXT("variable_name"), VariableName);

    // Load the blueprint
    UBlueprint* Blueprint = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(BlueprintPath));
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to load blueprint: %s"), *BlueprintPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_path` did not resolve to a UBlueprint via LoadObject. Verify the path with `list_assets` (asset_type='Blueprint'). Paths are case-sensitive and must include `/Game/` prefix; the .AssetName suffix is auto-handled."));
    }

    TArray<TSharedPtr<FJsonValue>> VariableArray;

    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        // If looking for specific variable, skip others
        if (bSpecificVariable && Variable.VarName.ToString() != VariableName)
        {
            continue;
        }

        TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
        VarObj->SetStringField(TEXT("name"), Variable.VarName.ToString());
        VarObj->SetStringField(TEXT("type"), Variable.VarType.PinCategory.ToString());
        VarObj->SetStringField(TEXT("sub_category"), Variable.VarType.PinSubCategory.ToString());
        VarObj->SetStringField(TEXT("default_value"), Variable.DefaultValue);
        VarObj->SetStringField(TEXT("friendly_name"), Variable.FriendlyName.IsEmpty() ? Variable.VarName.ToString() : Variable.FriendlyName);
        
        // Get tooltip from metadata (VarTooltip doesn't exist in UE 5.5)
        FString TooltipValue;
        if (Variable.HasMetaData(FBlueprintMetadata::MD_Tooltip))
        {
            TooltipValue = Variable.GetMetaData(FBlueprintMetadata::MD_Tooltip);
        }
        VarObj->SetStringField(TEXT("tooltip"), TooltipValue);
        
        VarObj->SetStringField(TEXT("category"), Variable.Category.ToString());

        // Property flags — round-trip parity with set_blueprint_variable_properties.
        // Each field here mirrors a setter parameter one-to-one so a get→set roundtrip is
        // lossless. See Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/BlueprintGraph/BPVariables.cpp
        // ::SetBlueprintVariableProperties for the matching write sites.
        const bool bIsEditable = (Variable.PropertyFlags & CPF_Edit) != 0;
        VarObj->SetBoolField(TEXT("is_editable"), bIsEditable);
        // is_public is the setter-side name for the same CPF_Edit flag — emit both so
        // callers can use either name on the round-trip path.
        VarObj->SetBoolField(TEXT("is_public"), bIsEditable);
        VarObj->SetBoolField(TEXT("is_blueprint_visible"), (Variable.PropertyFlags & CPF_BlueprintVisible) != 0);
        VarObj->SetBoolField(TEXT("is_blueprint_writable"), (Variable.PropertyFlags & CPF_BlueprintReadOnly) == 0);
        VarObj->SetBoolField(TEXT("is_editable_in_instance"), (Variable.PropertyFlags & CPF_DisableEditOnInstance) == 0);
        VarObj->SetBoolField(TEXT("is_config"), (Variable.PropertyFlags & CPF_Config) != 0);
        VarObj->SetBoolField(TEXT("expose_to_cinematics"), (Variable.PropertyFlags & CPF_Interp) != 0);

        // Replication: the setter splits this into two independent fields. Emit both;
        // keep the legacy `replication` key for back-compat (value == replication_condition).
        VarObj->SetBoolField(TEXT("replication_enabled"), (Variable.PropertyFlags & CPF_Net) != 0);
        VarObj->SetNumberField(TEXT("replication_condition"), (int32)Variable.ReplicationCondition);
        VarObj->SetNumberField(TEXT("replication"), (int32)Variable.ReplicationCondition);

        // Metadata-backed fields. The setter writes via SetMetaData/RemoveMetaData; the
        // round-trip-safe read is HasMetaData + GetMetaData. Missing keys emit empty
        // string / false so the JSON shape stays stable across variables.
        VarObj->SetBoolField(TEXT("expose_on_spawn"), Variable.HasMetaData(FBlueprintMetadata::MD_ExposeOnSpawn));
        VarObj->SetBoolField(TEXT("is_private"), Variable.HasMetaData(FBlueprintMetadata::MD_AllowPrivateAccess));
        VarObj->SetBoolField(TEXT("bitmask"), Variable.HasMetaData(FBlueprintMetadata::MD_Bitmask));

        static const FName NAME_UIMin(TEXT("UIMin"));
        static const FName NAME_UIMax(TEXT("UIMax"));
        static const FName NAME_ClampMin(TEXT("ClampMin"));
        static const FName NAME_ClampMax(TEXT("ClampMax"));
        static const FName NAME_Units(TEXT("Units"));
        VarObj->SetStringField(TEXT("slider_range_min"), Variable.HasMetaData(NAME_UIMin) ? Variable.GetMetaData(NAME_UIMin) : FString());
        VarObj->SetStringField(TEXT("slider_range_max"), Variable.HasMetaData(NAME_UIMax) ? Variable.GetMetaData(NAME_UIMax) : FString());
        VarObj->SetStringField(TEXT("value_range_min"), Variable.HasMetaData(NAME_ClampMin) ? Variable.GetMetaData(NAME_ClampMin) : FString());
        VarObj->SetStringField(TEXT("value_range_max"), Variable.HasMetaData(NAME_ClampMax) ? Variable.GetMetaData(NAME_ClampMax) : FString());
        VarObj->SetStringField(TEXT("units"), Variable.HasMetaData(NAME_Units) ? Variable.GetMetaData(NAME_Units) : FString());
        VarObj->SetStringField(TEXT("bitmask_enum"), Variable.HasMetaData(FBlueprintMetadata::MD_BitmaskEnum) ? Variable.GetMetaData(FBlueprintMetadata::MD_BitmaskEnum) : FString());

        VariableArray.Add(MakeShared<FJsonValueObject>(VarObj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_path"), BlueprintPath);
    
    if (bSpecificVariable)
    {
        ResultObj->SetStringField(TEXT("variable_name"), VariableName);
        if (VariableArray.Num() > 0)
        {
            ResultObj->SetObjectField(TEXT("variable"), VariableArray[0]->AsObject());
        }
        else
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Variable not found: %s"), *VariableName),
                EMCPErrorCode::VariableNotFound,
                TEXT("`variable_name` did not match any UPROPERTY-backed BP variable. Variable names match the BP's NewVariables array entry name (case-sensitive). Use `read_blueprint_content` to enumerate the Blueprint's variables."));
        }
    }
    else
    {
        ResultObj->SetArrayField(TEXT("variables"), VariableArray);
        ResultObj->SetNumberField(TEXT("variable_count"), VariableArray.Num());
    }

    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// GAP-044: serialize a node's pins (name/direction/type + linked-to connections) in the
// same shape HandleListNodePins emits, so bp_get_function_details(include_graph) exposes
// function-graph wiring pin-by-pin instead of only {name,class,title}.
static TArray<TSharedPtr<FJsonValue>> SerializeNodePins(UEdGraphNode* Node)
{
    TArray<TSharedPtr<FJsonValue>> PinArray;
    if (!Node) return PinArray;

    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->bHidden) continue;

        TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
        PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
        PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
        PinObj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());

        if (Pin->PinType.PinSubCategory != NAME_None)
        {
            PinObj->SetStringField(TEXT("sub_category"), Pin->PinType.PinSubCategory.ToString());
        }
        if (Pin->PinType.PinSubCategoryObject.IsValid())
        {
            PinObj->SetStringField(TEXT("sub_category_object"), Pin->PinType.PinSubCategoryObject->GetName());
        }

        PinObj->SetBoolField(TEXT("is_connected"), Pin->LinkedTo.Num() > 0);
        PinObj->SetNumberField(TEXT("num_connections"), Pin->LinkedTo.Num());

        if (!Pin->DefaultValue.IsEmpty())
        {
            PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
        }

        if (Pin->LinkedTo.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> ConnArray;
            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                if (!Linked || !Linked->GetOwningNodeUnchecked()) continue;
                TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
                ConnObj->SetStringField(TEXT("node_id"), Linked->GetOwningNode()->GetName());
                ConnObj->SetStringField(TEXT("pin_name"), Linked->PinName.ToString());
                ConnArray.Add(MakeShared<FJsonValueObject>(ConnObj));
            }
            PinObj->SetArrayField(TEXT("connections"), ConnArray);
        }

        PinArray.Add(MakeShared<FJsonValueObject>(PinObj));
    }
    return PinArray;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleGetBlueprintFunctionDetails(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_path` is required (string, full `/Game/...` asset path to a Blueprint). Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString FunctionName;
    bool bSpecificFunction = Params->TryGetStringField(TEXT("function_name"), FunctionName);

    bool bIncludeGraph = true;
    Params->TryGetBoolField(TEXT("include_graph"), bIncludeGraph);

    // Load the blueprint
    UBlueprint* Blueprint = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(BlueprintPath));
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to load blueprint: %s"), *BlueprintPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_path` did not resolve to a UBlueprint via LoadObject. Verify the path with `list_assets` (asset_type='Blueprint'). Paths are case-sensitive and must include `/Game/` prefix; the .AssetName suffix is auto-handled."));
    }

    TArray<TSharedPtr<FJsonValue>> FunctionArray;

    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (!Graph) continue;

        // If looking for specific function, skip others
        if (bSpecificFunction && Graph->GetName() != FunctionName)
        {
            continue;
        }

        TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
        FuncObj->SetStringField(TEXT("name"), Graph->GetName());
        FuncObj->SetStringField(TEXT("graph_type"), TEXT("Function"));

        // Get function signature from graph
        TArray<TSharedPtr<FJsonValue>> InputPins;
        TArray<TSharedPtr<FJsonValue>> OutputPins;

        // Find function entry and result nodes
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node)
            {
                if (Node->GetClass()->GetName().Contains(TEXT("FunctionEntry")))
                {
                    // Process input parameters
                    for (UEdGraphPin* Pin : Node->Pins)
                    {
                        if (Pin && Pin->Direction == EGPD_Output && Pin->PinName != TEXT("then"))
                        {
                            TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
                            PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
                            PinObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
                            InputPins.Add(MakeShared<FJsonValueObject>(PinObj));
                        }
                    }
                }
                else if (Node->GetClass()->GetName().Contains(TEXT("FunctionResult")))
                {
                    // Process output parameters
                    for (UEdGraphPin* Pin : Node->Pins)
                    {
                        if (Pin && Pin->Direction == EGPD_Input && Pin->PinName != TEXT("exec"))
                        {
                            TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
                            PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
                            PinObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
                            OutputPins.Add(MakeShared<FJsonValueObject>(PinObj));
                        }
                    }
                }
            }
        }

        FuncObj->SetArrayField(TEXT("input_parameters"), InputPins);
        FuncObj->SetArrayField(TEXT("output_parameters"), OutputPins);
        FuncObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

        // Include graph details if requested
        if (bIncludeGraph)
        {
            TArray<TSharedPtr<FJsonValue>> NodeArray;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node)
                {
                    TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
                    NodeObj->SetStringField(TEXT("name"), Node->GetName());
                    NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
                    NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
                    // GAP-044: emit pins + their linked-to connections so function-graph
                    // wiring is mappable pin-by-pin (mirrors bp_list_node_pins' pin shape).
                    NodeObj->SetArrayField(TEXT("pins"), SerializeNodePins(Node));
                    NodeArray.Add(MakeShared<FJsonValueObject>(NodeObj));
                }
            }
            FuncObj->SetArrayField(TEXT("graph_nodes"), NodeArray);
        }

        FunctionArray.Add(MakeShared<FJsonValueObject>(FuncObj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_path"), BlueprintPath);
    
    if (bSpecificFunction)
    {
        ResultObj->SetStringField(TEXT("function_name"), FunctionName);
        if (FunctionArray.Num() > 0)
        {
            ResultObj->SetObjectField(TEXT("function"), FunctionArray[0]->AsObject());
        }
        else
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Function not found: %s"), *FunctionName),
                EMCPErrorCode::FunctionNotFound,
                TEXT("`function_name` did not match any function graph on the Blueprint. Function names match the FunctionGraphs array entry name (case-sensitive). Use `list_blueprint_graphs` to enumerate."));
        }
    }
    else
    {
        ResultObj->SetArrayField(TEXT("functions"), FunctionArray);
        ResultObj->SetNumberField(TEXT("function_count"), FunctionArray.Num());
    }

    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ---------------------------------------------------------------------------
// get_mesh_bounds — return AABB for a static mesh asset or placed actor
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleGetMeshBounds(const TSharedPtr<FJsonObject>& Params)
{
    FBox BoundingBox(ForceInit);
    FString SourceDesc;

    FString MeshPath;
    FString ActorName;

    // Mode 1: asset path — raw mesh bounds (unscaled)
    if (Params->TryGetStringField(TEXT("mesh_path"), MeshPath))
    {
        UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(MeshPath));
        if (!Mesh)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Static mesh not found: %s"), *MeshPath),
                EMCPErrorCode::AssetNotFound,
                TEXT("`mesh_path` did not resolve to a UStaticMesh via LoadAsset. Verify with `list_assets` (asset_type='StaticMesh'). Paths are case-sensitive and must include `/Game/` prefix."));
        }
        BoundingBox = Mesh->GetBoundingBox();
        SourceDesc = MeshPath;
    }
    // Mode 2: actor in level — world-space bounds (includes scale)
    else if (Params->TryGetStringField(TEXT("actor_name"), ActorName))
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World)
        {
            return FMCPCommonUtils::CreateErrorResponse(
            TEXT("No editor world"),
            EMCPErrorCode::Internal,
            TEXT("The editor's world context returned null. The editor is mid-startup or shutdown. Retry once the editor is fully loaded."));
        }

        AActor* FoundActor = nullptr;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetName() == ActorName || It->GetActorLabel() == ActorName)
            {
                FoundActor = *It;
                break;
            }
        }
        if (!FoundActor)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Actor not found: %s"), *ActorName),
                EMCPErrorCode::ActorNotFound,
                TEXT("No actor in the editor world matches `actor_name`. Names are case-sensitive and match the UObject name or ActorLabel. Use `get_actors_in_level` or `find_actors_by_name` to discover."));
        }

        FVector Origin, Extent;
        FoundActor->GetActorBounds(false, Origin, Extent);
        BoundingBox = FBox(Origin - Extent, Origin + Extent);
        SourceDesc = ActorName;
    }
    else
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Provide either 'mesh_path' (asset) or 'actor_name' (placed actor)"),
            EMCPErrorCode::InvalidArgument,
            TEXT("This tool requires one of two mutually-exclusive params: `mesh_path` (full `/Game/...` path to a UStaticMesh, returns raw bounds) OR `actor_name` (placed-actor name, returns world-space bounds including scale). Pass exactly one."));
    }

    FVector Size = BoundingBox.GetSize();
    FVector Center = BoundingBox.GetCenter();
    FVector Extent = BoundingBox.GetExtent(); // half-size

    auto VecToJson = [](const FVector& V) -> TArray<TSharedPtr<FJsonValue>>
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(V.X));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
        return Arr;
    };

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("source"), SourceDesc);
    Result->SetArrayField(TEXT("min"), VecToJson(BoundingBox.Min));
    Result->SetArrayField(TEXT("max"), VecToJson(BoundingBox.Max));
    Result->SetArrayField(TEXT("center"), VecToJson(Center));
    Result->SetArrayField(TEXT("extent"), VecToJson(Extent));
    Result->SetArrayField(TEXT("size"), VecToJson(Size));
    Result->SetBoolField(TEXT("success"), true);
    return Result;
}

// ── list_blueprint_graphs ──────────────────────────────────────────────

static TSharedPtr<FJsonObject> MakeGraphEntry(
    UEdGraph* Graph,
    const FString& Category,
    const FString& ParentNode = FString(),
    const FString& ParentStateMachine = FString(),
    const FString& FromState = FString(),
    const FString& ToState = FString())
{
    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("name"), Graph->GetName());
    Entry->SetStringField(TEXT("type"), Graph->GetClass()->GetName());
    Entry->SetStringField(TEXT("category"), Category);
    Entry->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
    if (!ParentNode.IsEmpty())
    {
        Entry->SetStringField(TEXT("parent_node"), ParentNode);
    }
    if (!ParentStateMachine.IsEmpty())
    {
        Entry->SetStringField(TEXT("parent_state_machine"), ParentStateMachine);
    }
    if (!FromState.IsEmpty())
    {
        Entry->SetStringField(TEXT("from_state"), FromState);
    }
    if (!ToState.IsEmpty())
    {
        Entry->SetStringField(TEXT("to_state"), ToState);
    }
    return Entry;
}

static void EnumerateStateMachineGraphs(
    UAnimationStateMachineGraph* SMGraph,
    const FString& ParentNodeName,
    TArray<TSharedPtr<FJsonValue>>& OutGraphs)
{
    if (!SMGraph) return;

    // The state machine graph itself
    OutGraphs.Add(MakeShared<FJsonValueObject>(
        MakeGraphEntry(SMGraph, TEXT("state_machine"), ParentNodeName)));

    FString SMName = SMGraph->GetName();

    for (UEdGraphNode* Node : SMGraph->Nodes)
    {
        // State nodes — each has an inner AnimGraph
        if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
        {
            if (StateNode->BoundGraph)
            {
                OutGraphs.Add(MakeShared<FJsonValueObject>(
                    MakeGraphEntry(StateNode->BoundGraph, TEXT("state"),
                        FString(), SMName)));
            }
        }
        // Transition nodes — each has a condition rule graph
        else if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(Node))
        {
            if (TransNode->BoundGraph)
            {
                FString From = TransNode->GetPreviousState() ? TransNode->GetPreviousState()->GetStateName() : TEXT("?");
                FString To = TransNode->GetNextState() ? TransNode->GetNextState()->GetStateName() : TEXT("?");
                OutGraphs.Add(MakeShared<FJsonValueObject>(
                    MakeGraphEntry(TransNode->BoundGraph, TEXT("transition"),
                        FString(), SMName, From, To)));
            }
        }
    }
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleListBlueprintGraphs(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_path` is required (string, full `/Game/...` asset path to a Blueprint). Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprintByName(BlueprintPath);
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to load blueprint: %s"), *BlueprintPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_path` did not resolve to a UBlueprint via LoadObject. Verify the path with `list_assets` (asset_type='Blueprint'). Paths are case-sensitive."));
    }

    TArray<TSharedPtr<FJsonValue>> Graphs;

    // Ubergraph pages (EventGraph, etc.)
    for (UEdGraph* G : Blueprint->UbergraphPages)
    {
        if (G) Graphs.Add(MakeShared<FJsonValueObject>(MakeGraphEntry(G, TEXT("ubergraph"))));
    }

    // Function graphs (includes AnimGraph for AnimBPs)
    for (UEdGraph* G : Blueprint->FunctionGraphs)
    {
        if (G) Graphs.Add(MakeShared<FJsonValueObject>(MakeGraphEntry(G, TEXT("function"))));
    }

    // Delegate signature graphs
    for (UEdGraph* G : Blueprint->DelegateSignatureGraphs)
    {
        if (G) Graphs.Add(MakeShared<FJsonValueObject>(MakeGraphEntry(G, TEXT("delegate"))));
    }

    // Recursively discover state machine sub-graphs inside AnimGraphs and function graphs
    auto EnumerateSubGraphs = [&Graphs](const TArray<UEdGraph*>& GraphList)
    {
        for (UEdGraph* G : GraphList)
        {
            if (!G) continue;
            for (UEdGraphNode* Node : G->Nodes)
            {
                if (UAnimGraphNode_StateMachine* SM = Cast<UAnimGraphNode_StateMachine>(Node))
                {
                    EnumerateStateMachineGraphs(SM->EditorStateMachineGraph, Node->GetName(), Graphs);
                }
            }
        }
    };

    EnumerateSubGraphs(Blueprint->UbergraphPages);
    EnumerateSubGraphs(Blueprint->FunctionGraphs);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_path"), BlueprintPath);
    ResultObj->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    ResultObj->SetStringField(TEXT("parent_class"),
        Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : TEXT("None"));
    ResultObj->SetArrayField(TEXT("graphs"), Graphs);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ── reparent_blueprint ─────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleReparentBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (!Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_path` is required (string, full `/Game/...` asset path to a Blueprint). Use `list_assets` with `asset_type='Blueprint'` to discover."));
    }

    FString NewParentClassName;
    if (!Params->TryGetStringField(TEXT("new_parent_class"), NewParentClassName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'new_parent_class' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`new_parent_class` is required (string, the target parent class for reparenting). Accepts short name, FQN path (`/Script/Module.Class`), or BP path. Reparenting must preserve the inherited contract — verify the new parent is BP-compatible with `get_class_properties`."));
    }

    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprintByName(BlueprintPath);
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to load blueprint: %s"), *BlueprintPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_path` did not resolve to a UBlueprint via LoadObject. Verify the path with `list_assets` (asset_type='Blueprint'). Paths are case-sensitive."));
    }

    // Find the new parent class — try by path first, then by name
    UClass* NewParentClass = LoadObject<UClass>(nullptr, *NewParentClassName);
    if (!NewParentClass)
    {
        NewParentClass = FindFirstObject<UClass>(*NewParentClassName, EFindFirstObjectOptions::NativeFirst);
    }
    if (!NewParentClass)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not find class: %s"), *NewParentClassName),
            EMCPErrorCode::ClassNotLoaded,
            TEXT("`new_parent_class` did not resolve via LoadObject<UClass> or FindFirstObject. Accepts BP `_C`-suffixed paths, native FQN paths (`/Script/Module.Class`), or short names. For C++ classes, ensure the owning module is loaded; for BP parents, the BP must be compiled."));
    }

    // Refuse to reparent when the CURRENT parent failed to resolve. A null
    // ParentClass means the old parent asset was renamed/removed out from under
    // this child; reparenting through that state compiles with no parent context
    // and the SCS reconcile DISCARDS every BP-added component (has_scs:false,
    // components_count:0 afterward). The editor-UI reparent preserves the SCS
    // because it always goes class→class; this MCP path through a null parent
    // does not. Refuse rather than silently destroy the child's components —
    // the caller should rename the parent asset (+ fixup_redirectors) instead,
    // or re-add the SCS components after. See docs/bugs/mcp.md.
    if (!Blueprint->ParentClass)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to reparent '%s': its current parent class is unresolved (null)"), *BlueprintPath),
            EMCPErrorCode::Internal,
            TEXT("The Blueprint's current ParentClass is null — its old parent asset was likely renamed or removed. Reparenting from a null parent silently DISCARDS the child's entire SimpleConstructionScript (all BP-added components are lost). Fix the broken parent reference first: rename the renamed parent back (or `fixup_redirectors`) so the child reloads with a valid parent, then reparent. If you must proceed, capture the SCS component list first and re-add the components afterward."));
    }

    FString OldParentName = Blueprint->ParentClass->GetName();

    Blueprint->ParentClass = NewParentClass;
    FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_path"), BlueprintPath);
    ResultObj->SetStringField(TEXT("old_parent_class"), OldParentName);
    ResultObj->SetStringField(TEXT("new_parent_class"), NewParentClass->GetName());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ── list_skeletons ─────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleListSkeletons(const TSharedPtr<FJsonObject>& Params)
{
    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(USkeleton::StaticClass()->GetClassPathName());

    TArray<FAssetData> SkeletonAssets;
    AssetRegistry.GetAssets(Filter, SkeletonAssets);

    TArray<TSharedPtr<FJsonValue>> SkeletonArray;
    for (const FAssetData& Asset : SkeletonAssets)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        Entry->SetStringField(TEXT("path"), Asset.GetObjectPathString());
        Entry->SetStringField(TEXT("package_path"), Asset.PackagePath.ToString());
        SkeletonArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("skeletons"), SkeletonArray);
    ResultObj->SetNumberField(TEXT("count"), SkeletonAssets.Num());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ── list_anim_sequences ────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleListAnimSequences(const TSharedPtr<FJsonObject>& Params)
{
    FString SkeletonPath;
    Params->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);

    USkeleton* FilterSkeleton = nullptr;
    if (!SkeletonPath.IsEmpty())
    {
        FilterSkeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
        if (!FilterSkeleton)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Could not load skeleton: %s"), *SkeletonPath),
                EMCPErrorCode::AssetNotFound,
                TEXT("`skeleton_path` did not resolve to a USkeleton via LoadObject. Verify with `list_skeletons`. Paths are case-sensitive."));
        }
    }

    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());

    TArray<FAssetData> AnimAssets;
    AssetRegistry.GetAssets(Filter, AnimAssets);

    TArray<TSharedPtr<FJsonValue>> SequenceArray;
    for (const FAssetData& Asset : AnimAssets)
    {
        // Filter by skeleton compatibility if requested
        if (FilterSkeleton)
        {
            FAssetDataTagMapSharedView::FFindTagResult SkeletonTag = Asset.TagsAndValues.FindTag(TEXT("Skeleton"));
            if (SkeletonTag.IsSet())
            {
                FString AssetSkeletonPath = SkeletonTag.GetValue();
                if (!AssetSkeletonPath.Contains(FilterSkeleton->GetName()))
                {
                    continue;
                }
            }
            else
            {
                continue;
            }
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        Entry->SetStringField(TEXT("path"), Asset.GetObjectPathString());

        // Duration from tag if available
        FAssetDataTagMapSharedView::FFindTagResult DurationTag = Asset.TagsAndValues.FindTag(TEXT("SequenceLength"));
        if (DurationTag.IsSet())
        {
            Entry->SetStringField(TEXT("duration"), DurationTag.GetValue());
        }

        SequenceArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("anim_sequences"), SequenceArray);
    ResultObj->SetNumberField(TEXT("count"), SequenceArray.Num());
    if (FilterSkeleton)
    {
        ResultObj->SetStringField(TEXT("skeleton_filter"), FilterSkeleton->GetPathName());
    }
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ── create_blend_space ─────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleCreateBlendSpace(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName;
    if (!Params->TryGetStringField(TEXT("name"), AssetName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`name` is required (string). The semantics depend on the handler — typically the new asset's short name (`MyBP`) or full asset path (`/Game/Folder/MyBP`). Path-form auto-creates intermediate folders."));
    }

    FString SkeletonPath;
    if (!Params->TryGetStringField(TEXT("skeleton_path"), SkeletonPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'skeleton_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`skeleton_path` is required (string, full `/Game/...` asset path to a USkeleton). Use `list_skeletons` to discover existing skeletons."));
    }

    USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
    if (!Skeleton)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not load skeleton: %s"), *SkeletonPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`skeleton_path` did not resolve to a USkeleton via LoadObject. Verify with `list_skeletons`. Paths are case-sensitive."));
    }

    // Determine 1D vs 2D (default 1D) and aim offset vs regular
    bool bIs2D = false;
    bool bIsAimOffset = false;
    Params->TryGetBoolField(TEXT("is_2d"), bIs2D);
    Params->TryGetBoolField(TEXT("is_aim_offset"), bIsAimOffset);

    // Asset path (default to /Game/BlendSpaces/)
    FString PackagePath = TEXT("/Game/BlendSpaces");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);

    // Create the blend space via factory
    UBlendSpace* BlendSpace = nullptr;
    IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();

    if (bIsAimOffset)
    {
        if (bIs2D)
        {
            UAimOffsetBlendSpaceFactoryNew* Factory = NewObject<UAimOffsetBlendSpaceFactoryNew>();
            Factory->TargetSkeleton = Skeleton;
            BlendSpace = Cast<UBlendSpace>(
                AssetTools.CreateAsset(AssetName, PackagePath,
                    UAimOffsetBlendSpace::StaticClass(), Factory));
        }
        else
        {
            UAimOffsetBlendSpaceFactory1D* Factory = NewObject<UAimOffsetBlendSpaceFactory1D>();
            Factory->TargetSkeleton = Skeleton;
            BlendSpace = Cast<UBlendSpace>(
                AssetTools.CreateAsset(AssetName, PackagePath,
                    UAimOffsetBlendSpace1D::StaticClass(), Factory));
        }
    }
    else if (bIs2D)
    {
        UBlendSpaceFactoryNew* Factory = NewObject<UBlendSpaceFactoryNew>();
        Factory->TargetSkeleton = Skeleton;
        BlendSpace = Cast<UBlendSpace>(
            AssetTools.CreateAsset(AssetName, PackagePath,
                UBlendSpace::StaticClass(), Factory));
    }
    else
    {
        UBlendSpaceFactory1D* Factory = NewObject<UBlendSpaceFactory1D>();
        Factory->TargetSkeleton = Skeleton;
        BlendSpace = Cast<UBlendSpace>(
            AssetTools.CreateAsset(AssetName, PackagePath,
                UBlendSpace1D::StaticClass(), Factory));
    }

    if (!BlendSpace)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Failed to create blend space asset"),
            EMCPErrorCode::Internal,
            TEXT("AssetTools.CreateAsset returned nullptr for UBlendSpace creation. Common causes: target path is read-only or already occupied; AssetRegistry is mid-scan. Pick a clean destination, or call `delete_asset` on an existing collision, then retry."));
    }

    BlendSpace->PreEditChange(nullptr);

    // Configure X axis (const_cast safe — we own this asset, property is EditAnywhere)
    const TSharedPtr<FJsonObject>* XAxisObj = nullptr;
    if (Params->TryGetObjectField(TEXT("x_axis"), XAxisObj))
    {
        FBlendParameter& XParam = const_cast<FBlendParameter&>(BlendSpace->GetBlendParameter(0));
        FString AxisName;
        if ((*XAxisObj)->TryGetStringField(TEXT("name"), AxisName))
        {
            XParam.DisplayName = AxisName;
        }
        double Min = 0, Max = 100;
        if ((*XAxisObj)->TryGetNumberField(TEXT("min"), Min)) XParam.Min = (float)Min;
        if ((*XAxisObj)->TryGetNumberField(TEXT("max"), Max)) XParam.Max = (float)Max;
        double GridNum = 4;
        if ((*XAxisObj)->TryGetNumberField(TEXT("grid_divisions"), GridNum)) XParam.GridNum = (int32)GridNum;
    }

    // Configure Y axis (only for 2D)
    if (bIs2D)
    {
        const TSharedPtr<FJsonObject>* YAxisObj = nullptr;
        if (Params->TryGetObjectField(TEXT("y_axis"), YAxisObj))
        {
            FBlendParameter& YParam = const_cast<FBlendParameter&>(BlendSpace->GetBlendParameter(1));
            FString AxisName;
            if ((*YAxisObj)->TryGetStringField(TEXT("name"), AxisName))
            {
                YParam.DisplayName = AxisName;
            }
            double Min = 0, Max = 100;
            if ((*YAxisObj)->TryGetNumberField(TEXT("min"), Min)) YParam.Min = (float)Min;
            if ((*YAxisObj)->TryGetNumberField(TEXT("max"), Max)) YParam.Max = (float)Max;
            double GridNum = 4;
            if ((*YAxisObj)->TryGetNumberField(TEXT("grid_divisions"), GridNum)) YParam.GridNum = (int32)GridNum;
        }
    }

    // Add initial samples if provided
    const TArray<TSharedPtr<FJsonValue>>* SamplesArray = nullptr;
    int32 SamplesAdded = 0;
    if (Params->TryGetArrayField(TEXT("samples"), SamplesArray))
    {
        for (const TSharedPtr<FJsonValue>& SampleVal : *SamplesArray)
        {
            const TSharedPtr<FJsonObject>& SampleObj = SampleVal->AsObject();
            if (!SampleObj.IsValid()) continue;

            FString AnimPath;
            if (!SampleObj->TryGetStringField(TEXT("anim_sequence"), AnimPath)) continue;

            UAnimSequence* AnimSeq = LoadObject<UAnimSequence>(nullptr, *AnimPath);
            if (!AnimSeq) continue;

            double X = 0, Y = 0;
            SampleObj->TryGetNumberField(TEXT("x"), X);
            SampleObj->TryGetNumberField(TEXT("y"), Y);

            FVector SamplePos((float)X, (float)Y, 0.0f);
            BlendSpace->AddSample(AnimSeq, SamplePos);
            SamplesAdded++;
        }
    }

    // CRITICAL: UBlendSpace::AddSample only mutates SampleData — it does NOT
    // rebuild the runtime triangulation/segment data (`GridSamples` and
    // `BlendSpaceData.Triangles`/`Segments`). Without ResampleData the asset
    // serializes with empty derived data; runtime blending reads nothing and
    // the AI/animgraph nodes silently produce T-poses or frozen samples until
    // the asset is opened in the editor (which calls ResampleData in its
    // Construct) and re-saved. This is what SBlendSpaceEditor::OnSampleAdded
    // does after every UI-driven sample add (Engine/Source/Editor/Persona/
    // Private/SAnimationBlendSpace.cpp:194) — match it here.
    BlendSpace->ResampleData();
    BlendSpace->PostEditChange();
    BlendSpace->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(BlendSpace->GetPathName(), false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Created blend space %s but saving it to disk failed (UEditorAssetLibrary::SaveAsset returned false)"), *BlendSpace->GetPathName()),
            EMCPErrorCode::Internal,
            TEXT("The blend space was created and modified in memory (package is dirty) but the save-to-disk call returned false — the change was NOT persisted. Common causes: the editor is running -unattended (EditorAssetSubsystem save paths no-op), the file is read-only / not checked out, or the package failed validation. Save manually in the editor, or re-run once the editor is interactive."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("name"), BlendSpace->GetName());
    ResultObj->SetStringField(TEXT("path"), BlendSpace->GetPathName());
    FString TypeStr = bIsAimOffset
        ? (bIs2D ? TEXT("AimOffsetBlendSpace") : TEXT("AimOffsetBlendSpace1D"))
        : (bIs2D ? TEXT("BlendSpace") : TEXT("BlendSpace1D"));
    ResultObj->SetStringField(TEXT("type"), TypeStr);
    ResultObj->SetNumberField(TEXT("samples_added"), SamplesAdded);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ── add_blend_space_sample ─────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleAddBlendSpaceSample(const TSharedPtr<FJsonObject>& Params)
{
    FString BlendSpacePath;
    if (!Params->TryGetStringField(TEXT("blend_space_path"), BlendSpacePath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blend_space_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blend_space_path` is required (string, full `/Game/...` asset path to a UBlendSpace). Use `list_blend_spaces` to discover existing blend spaces."));
    }

    FString AnimPath;
    if (!Params->TryGetStringField(TEXT("anim_sequence"), AnimPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'anim_sequence' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`anim_sequence` is required (string, full `/Game/...` asset path to a UAnimSequence). Use `list_anim_sequences` to discover existing animations."));
    }

    UBlendSpace* BlendSpace = LoadObject<UBlendSpace>(nullptr, *BlendSpacePath);
    if (!BlendSpace)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not load blend space: %s"), *BlendSpacePath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blend_space_path` did not resolve to a UBlendSpace via LoadObject. Verify with `list_blend_spaces`. Paths are case-sensitive."));
    }

    UAnimSequence* AnimSeq = LoadObject<UAnimSequence>(nullptr, *AnimPath);
    if (!AnimSeq)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not load anim sequence: %s"), *AnimPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`anim_sequence` did not resolve to a UAnimSequence via LoadObject. Verify with `list_anim_sequences`. Paths are case-sensitive."));
    }

    double X = 0, Y = 0;
    Params->TryGetNumberField(TEXT("x"), X);
    Params->TryGetNumberField(TEXT("y"), Y);

    // dry_run: every preflight ran (paths, blend space load, anim sequence
    // load). Skip PreEditChange + AddSample + ResampleData + PostEditChange +
    // MarkPackageDirty + SaveAsset. Diff shape per todo/13 phase 4:
    // samples_added[]. The would-be sample index is deterministic (AddSample
    // appends → would_be = current GetNumberOfBlendSamples()), so dry-run can
    // report the same index the apply call would return. Field names mirror
    // the read_blend_space per-sample shape (index/x/y/animation/animation_name)
    // so a read → add dry-run round-trip is schema-aligned.
    if (FMCPCommonUtils::ParseDryRun(Params))
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("blend_space"), BlendSpace->GetPathName());
        Entry->SetNumberField(TEXT("would_be_index"), BlendSpace->GetNumberOfBlendSamples());
        Entry->SetStringField(TEXT("animation"), AnimSeq->GetPathName());
        Entry->SetStringField(TEXT("animation_name"), AnimSeq->GetName());
        Entry->SetNumberField(TEXT("x"), X);
        Entry->SetNumberField(TEXT("y"), Y);

        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
        TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
        Diff->SetArrayField(TEXT("samples_added"), Arr);
        return FMCPCommonUtils::CreateDryRunResponse(Diff);
    }

    FVector SamplePos((float)X, (float)Y, 0.0f);
    BlendSpace->PreEditChange(nullptr);
    int32 SampleIndex = BlendSpace->AddSample(AnimSeq, SamplePos);

    // ResampleData rebuilds GridSamples and BlendSpaceData (runtime triangulation).
    // Without this call the asset saves with empty derived data and runtime blending
    // silently produces T-poses. See HandleCreateBlendSpace for the full explanation.
    BlendSpace->ResampleData();
    BlendSpace->PostEditChange();
    BlendSpace->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(BlendSpace->GetPathName(), false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Added sample to %s but saving it to disk failed (UEditorAssetLibrary::SaveAsset returned false)"), *BlendSpace->GetPathName()),
            EMCPErrorCode::Internal,
            TEXT("The sample was added and the blend space rebuilt in memory (package is dirty) but the save-to-disk call returned false — the change was NOT persisted. Common causes: -unattended editor (EditorAssetSubsystem save paths no-op), read-only / un-checked-out file, or package validation failure. Save manually, or re-run interactively."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blend_space"), BlendSpace->GetPathName());
    ResultObj->SetNumberField(TEXT("sample_index"), SampleIndex);
    ResultObj->SetStringField(TEXT("anim_sequence"), AnimSeq->GetName());
    ResultObj->SetNumberField(TEXT("x"), X);
    ResultObj->SetNumberField(TEXT("y"), Y);
    ResultObj->SetNumberField(TEXT("total_samples"), BlendSpace->GetNumberOfBlendSamples());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ── remove_blend_space_sample ──────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleRemoveBlendSpaceSample(const TSharedPtr<FJsonObject>& Params)
{
    FString BlendSpacePath;
    if (!Params->TryGetStringField(TEXT("blend_space_path"), BlendSpacePath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blend_space_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blend_space_path` is required (string, full `/Game/...` asset path to a UBlendSpace). Use `list_blend_spaces` to discover existing blend spaces."));
    }

    double SampleIndexD = -1;
    if (!Params->TryGetNumberField(TEXT("sample_index"), SampleIndexD))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'sample_index' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`sample_index` is required (integer, zero-based within the blend space's Samples array). Use `read_blend_space` to enumerate samples — each entry's `index` field is the value to pass."));
    }
    int32 SampleIndex = (int32)SampleIndexD;

    UBlendSpace* BlendSpace = LoadObject<UBlendSpace>(nullptr, *BlendSpacePath);
    if (!BlendSpace)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not load blend space: %s"), *BlendSpacePath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blend_space_path` did not resolve to a UBlendSpace via LoadObject. Verify with `list_blend_spaces`. Paths are case-sensitive."));
    }

    if (!BlendSpace->IsValidBlendSampleIndex(SampleIndex))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Invalid sample index %d (blend space has %d samples)"), SampleIndex, BlendSpace->GetNumberOfBlendSamples()),
            EMCPErrorCode::OutOfRange,
            TEXT("`sample_index` must be in [0, total_samples). Use `read_blend_space` to see the current sample count. The bound shifts after each add/remove — re-read if you're chaining mutations."));
    }

    // dry_run: every preflight ran (path, sample_index parse + range check via
    // IsValidBlendSampleIndex). Skip PreEditChange + DeleteSample + ResampleData
    // + PostEditChange + MarkPackageDirty + SaveAsset. Diff shape: samples_removed[]
    // mirroring samples_added[] (and read_blend_space's per-sample entry shape).
    // We capture the existing sample's animation + position via GetBlendSample
    // — same API the read path uses — so the diff shows what's about to be lost.
    if (FMCPCommonUtils::ParseDryRun(Params))
    {
        const FBlendSample& SampleToRemove = BlendSpace->GetBlendSample(SampleIndex);

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("blend_space"), BlendSpace->GetPathName());
        Entry->SetNumberField(TEXT("sample_index"), SampleIndex);
        Entry->SetNumberField(TEXT("x"), SampleToRemove.SampleValue.X);
        Entry->SetNumberField(TEXT("y"), SampleToRemove.SampleValue.Y);
        if (SampleToRemove.Animation)
        {
            Entry->SetStringField(TEXT("animation"), SampleToRemove.Animation->GetPathName());
            Entry->SetStringField(TEXT("animation_name"), SampleToRemove.Animation->GetName());
        }

        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
        TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
        Diff->SetArrayField(TEXT("samples_removed"), Arr);
        return FMCPCommonUtils::CreateDryRunResponse(Diff);
    }

    BlendSpace->PreEditChange(nullptr);
    BlendSpace->DeleteSample(SampleIndex);
    // ResampleData rebuilds GridSamples and BlendSpaceData (runtime triangulation).
    // Without this call the asset saves with stale derived data — one of the removed
    // sample's grid cells lingers in GridSamples until the asset is opened and re-saved
    // via the editor. See HandleCreateBlendSpace for the full explanation.
    BlendSpace->ResampleData();
    BlendSpace->PostEditChange();
    BlendSpace->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(BlendSpace->GetPathName(), false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Removed sample from %s but saving it to disk failed (UEditorAssetLibrary::SaveAsset returned false)"), *BlendSpace->GetPathName()),
            EMCPErrorCode::Internal,
            TEXT("The sample was removed and the blend space rebuilt in memory (package is dirty) but the save-to-disk call returned false — the change was NOT persisted. Common causes: -unattended editor (EditorAssetSubsystem save paths no-op), read-only / un-checked-out file, or package validation failure. Save manually, or re-run interactively."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blend_space"), BlendSpace->GetPathName());
    ResultObj->SetNumberField(TEXT("removed_index"), SampleIndex);
    ResultObj->SetNumberField(TEXT("remaining_samples"), BlendSpace->GetNumberOfBlendSamples());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ── set_anim_sequence_property ─────────────────────────────────────────

// EAdditiveAnimationType is declared `enum EAdditiveAnimationType : int` with `UENUM()` —
// ECppForm::Regular per Runtime/Engine/Public/Animation/AnimTypes.h:574-583, so
// StaticEnum<...>->GetNameStringByValue returns the raw FName ("AAT_None",
// "AAT_LocalSpaceBase", "AAT_RotationOffsetMeshSpace") — see
// Runtime/CoreUObject/Private/UObject/Enum.cpp:759-778. The handler's success
// response emits those raw FNames, so the parser must accept them too for
// `set → echo response back to set` round-trip parity. Same shape applies to
// EAdditiveBasePoseType (Runtime/Engine/Classes/Animation/AnimEnums.h:49-62).
static bool TryParseAdditiveAnimType(const FString& Str, EAdditiveAnimationType& Out)
{
    if (Str.Equals(TEXT("None"), ESearchCase::IgnoreCase) ||
        Str.Equals(TEXT("NoAdditive"), ESearchCase::IgnoreCase) ||
        Str.Equals(TEXT("AAT_None"), ESearchCase::IgnoreCase))
    {
        Out = AAT_None;
        return true;
    }
    if (Str.Equals(TEXT("LocalSpace"), ESearchCase::IgnoreCase) ||
        Str.Equals(TEXT("AAT_LocalSpaceBase"), ESearchCase::IgnoreCase))
    {
        Out = AAT_LocalSpaceBase;
        return true;
    }
    if (Str.Equals(TEXT("MeshSpace"), ESearchCase::IgnoreCase) ||
        Str.Equals(TEXT("AAT_RotationOffsetMeshSpace"), ESearchCase::IgnoreCase))
    {
        Out = AAT_RotationOffsetMeshSpace;
        return true;
    }
    return false;
}

static bool TryParseAdditiveBasePoseType(const FString& Str, EAdditiveBasePoseType& Out)
{
    if (Str.Equals(TEXT("None"), ESearchCase::IgnoreCase) ||
        Str.Equals(TEXT("ABPT_None"), ESearchCase::IgnoreCase))
    {
        Out = ABPT_None;
        return true;
    }
    // ABPT_AnimFrame ("Selected animation frame") — `ReferencePose` is a
    // pre-existing alias kept for back-compat; its surface meaning is
    // suspicious (a #RESEARCH note tracks this) but changing it is a
    // separate behavior bundle. The engine-canonical "ABPT_AnimFrame"
    // alias is the unambiguous form.
    if (Str.Equals(TEXT("AnimFrame"), ESearchCase::IgnoreCase) ||
        Str.Equals(TEXT("ReferencePose"), ESearchCase::IgnoreCase) ||
        Str.Equals(TEXT("ABPT_AnimFrame"), ESearchCase::IgnoreCase))
    {
        Out = ABPT_AnimFrame;
        return true;
    }
    if (Str.Equals(TEXT("AnimScale"), ESearchCase::IgnoreCase) ||
        Str.Equals(TEXT("AnimationScaled"), ESearchCase::IgnoreCase) ||
        Str.Equals(TEXT("ABPT_AnimScaled"), ESearchCase::IgnoreCase))
    {
        Out = ABPT_AnimScaled;
        return true;
    }
    // ABPT_RefPose ("Skeleton Reference Pose") — `SelectedAnimationFrame`
    // / `SelectedAnimFrame` are pre-existing aliases kept for back-compat;
    // the engine-canonical "ABPT_RefPose" alias is the unambiguous form.
    if (Str.Equals(TEXT("SelectedAnimationFrame"), ESearchCase::IgnoreCase) ||
        Str.Equals(TEXT("SelectedAnimFrame"), ESearchCase::IgnoreCase) ||
        Str.Equals(TEXT("ABPT_RefPose"), ESearchCase::IgnoreCase))
    {
        Out = ABPT_RefPose;
        return true;
    }
    if (Str.Equals(TEXT("LocalAnimFrame"), ESearchCase::IgnoreCase) ||
        Str.Equals(TEXT("ABPT_LocalAnimFrame"), ESearchCase::IgnoreCase))
    {
        Out = ABPT_LocalAnimFrame;
        return true;
    }
    return false;
}

#define MCP_INVALID_ADDITIVE_ANIM_TYPE_MSG TEXT("Invalid 'additive_anim_type' value '%s'. Valid: None, NoAdditive, AAT_None, LocalSpace, AAT_LocalSpaceBase, MeshSpace, AAT_RotationOffsetMeshSpace.")
#define MCP_INVALID_BASE_POSE_TYPE_MSG TEXT("Invalid 'base_pose_type' value '%s'. Valid: None, ABPT_None, AnimFrame, ReferencePose, ABPT_AnimFrame, AnimScale, AnimationScaled, ABPT_AnimScaled, SelectedAnimationFrame, SelectedAnimFrame, ABPT_RefPose, LocalAnimFrame, ABPT_LocalAnimFrame.")

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleSetAnimSequenceProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString AnimPath;
    if (!Params->TryGetStringField(TEXT("anim_path"), AnimPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'anim_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`anim_path` is required (string, full `/Game/...` asset path to a UAnimSequence). Use `list_anim_sequences` to discover."));
    }

    UAnimSequence* AnimSeq = LoadObject<UAnimSequence>(nullptr, *AnimPath);
    if (!AnimSeq)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not load anim sequence: %s"), *AnimPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`anim_sequence` did not resolve to a UAnimSequence via LoadObject. Verify with `list_anim_sequences`. Paths are case-sensitive."));
    }

    // Validate-before-mutate. UObject::PreEditChange (Runtime/CoreUObject/Private/UObject/Obj.cpp:483)
    // calls Modify(true) which dirties the package and snapshots state into the active undo
    // transaction; every PreEditChange must be balanced by PostEditChange. Parsing and
    // existence checks run above the edit envelope so an early-return error path cannot
    // leave the asset half-edited (dirty package, orphan transaction record, missing
    // FCoreUObjectDelegates::OnObjectPropertyChanged broadcast).

    // Additive anim type — engine ref Runtime/Engine/Public/Animation/AnimTypes.h:574-583.
    bool bHasAdditiveAnimType = false;
    EAdditiveAnimationType NewAdditiveAnimType = AAT_None;
    FString AdditiveType;
    if (Params->TryGetStringField(TEXT("additive_anim_type"), AdditiveType))
    {
        if (!TryParseAdditiveAnimType(AdditiveType, NewAdditiveAnimType))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(MCP_INVALID_ADDITIVE_ANIM_TYPE_MSG, *AdditiveType),
                EMCPErrorCode::InvalidArgument,
                TEXT("`additive_anim_type` must be one of: `None`, `LocalSpaceBase`, `MeshSpaceBase`, `MeshSpaceRotation` (case-insensitive). See Engine/Source/Runtime/Engine/Public/Animation/AnimTypes.h for the EAdditiveAnimationType enum."));
        }
        bHasAdditiveAnimType = true;
    }

    // Base pose type — engine ref Runtime/Engine/Classes/Animation/AnimEnums.h:49-62.
    bool bHasBasePoseType = false;
    EAdditiveBasePoseType NewRefPoseType = ABPT_None;
    FString BasePoseType;
    if (Params->TryGetStringField(TEXT("base_pose_type"), BasePoseType))
    {
        if (!TryParseAdditiveBasePoseType(BasePoseType, NewRefPoseType))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(MCP_INVALID_BASE_POSE_TYPE_MSG, *BasePoseType),
                EMCPErrorCode::InvalidArgument,
                TEXT("`base_pose_type` must be one of: `None`, `LocalAnimFrame`, `AnimScaled`, `AnimFrame`, `RefFrame` (case-insensitive). See Engine/Source/Runtime/Engine/Classes/Animation/AnimEnums.h for the EAdditiveBasePoseType enum."));
        }
        bHasBasePoseType = true;
    }

    // Base pose animation reference
    bool bHasBasePoseAnim = false;
    UAnimSequence* NewRefPoseSeq = nullptr;
    FString BasePoseAnim;
    if (Params->TryGetStringField(TEXT("base_pose_anim"), BasePoseAnim))
    {
        NewRefPoseSeq = LoadObject<UAnimSequence>(nullptr, *BasePoseAnim);
        if (!NewRefPoseSeq)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Could not load base pose animation: %s"), *BasePoseAnim),
                EMCPErrorCode::AssetNotFound,
                TEXT("`base_pose_anim` did not resolve to a UAnimSequence via LoadObject. Use `list_anim_sequences` to discover. Paths are case-sensitive."));
        }
        bHasBasePoseAnim = true;
    }

    // Reference frame index
    bool bHasRefFrameIndex = false;
    int32 NewRefFrameIndex = 0;
    double FrameIdx;
    if (Params->TryGetNumberField(TEXT("ref_frame_index"), FrameIdx))
    {
        NewRefFrameIndex = (int32)FrameIdx;
        bHasRefFrameIndex = true;
    }

    if (!bHasAdditiveAnimType && !bHasBasePoseType && !bHasBasePoseAnim && !bHasRefFrameIndex)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("No recognized properties provided"),
            EMCPErrorCode::InvalidArgument,
            TEXT("None of the keys in `properties` matched any FProperty on the target's class. Property names match the C++ UPROPERTY identifier (case-sensitive). Use `get_class_properties` to enumerate the valid property names for this target."));
    }

    // All parses + existence checks succeeded; open the edit transaction.
    AnimSeq->PreEditChange(nullptr);

    if (bHasAdditiveAnimType)
    {
        AnimSeq->AdditiveAnimType = NewAdditiveAnimType;
    }
    if (bHasBasePoseType)
    {
        AnimSeq->RefPoseType = NewRefPoseType;
    }
    if (bHasBasePoseAnim)
    {
        AnimSeq->RefPoseSeq = NewRefPoseSeq;
    }
    if (bHasRefFrameIndex)
    {
        AnimSeq->RefFrameIndex = NewRefFrameIndex;
    }

    // Fire PostEditChangeProperty with a SPECIFIC property — not PostEditChange()
    // with nullptr. UAnimSequence::PostEditChangeProperty gates the critical
    // derived-data rebuild on matching one of: AdditiveAnimType / RefPoseSeq /
    // RefPoseType / RefFrameIndex (Engine/Source/Runtime/Engine/Private/Animation/
    // AnimSequence.cpp:1308-1340). When `Property == nullptr`, the `if (Property)`
    // branch is skipped → `bAdditiveSettingsChanged` stays false → `bNeedPostProcess`
    // stays false → `ClearAllCompressionData()` + `BeginCacheDerivedDataForCurrentPlatform()`
    // are NOT called. Runtime then reads STALE compressed data under the new
    // AdditiveAnimType tag and the additive blend produces garbage.
    //
    // Picking AdditiveAnimType is sufficient: all four matching branches set
    // `bAdditiveSettingsChanged = true` and run the same clear+rebuild path.
    // Firing once (rather than once per property) amortizes the expensive
    // BeginCacheDerivedDataForCurrentPlatform call.
    FProperty* AdditiveProp = FindFProperty<FProperty>(
        UAnimSequence::StaticClass(),
        GET_MEMBER_NAME_CHECKED(UAnimSequence, AdditiveAnimType));
    if (AdditiveProp)
    {
        FPropertyChangedEvent ChangeEvent(AdditiveProp, EPropertyChangeType::ValueSet);
        AnimSeq->PostEditChangeProperty(ChangeEvent);
    }
    else
    {
        // Defensive fallback — should be unreachable (the property is declared
        // on UAnimSequence in the engine). Keeps the old no-op behavior rather
        // than crashing on a null property.
        AnimSeq->PostEditChange();
    }
    AnimSeq->MarkPackageDirty();

    if (!UEditorAssetLibrary::SaveAsset(AnimSeq->GetPathName(), /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Anim sequence properties mutated in-memory but failed to persist to disk: %s"), *AnimSeq->GetPathName()),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not written. SaveAsset no-ops while PIE is active or when the package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the change exists in-memory but will be lost on editor restart."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("anim_path"), AnimSeq->GetPathName());
    ResultObj->SetStringField(TEXT("additive_anim_type"),
        StaticEnum<EAdditiveAnimationType>()->GetNameStringByValue((int64)AnimSeq->AdditiveAnimType));
    ResultObj->SetStringField(TEXT("base_pose_type"),
        StaticEnum<EAdditiveBasePoseType>()->GetNameStringByValue((int64)AnimSeq->RefPoseType));
    if (AnimSeq->RefPoseSeq)
    {
        ResultObj->SetStringField(TEXT("base_pose_anim"), AnimSeq->RefPoseSeq->GetPathName());
    }
    ResultObj->SetNumberField(TEXT("ref_frame_index"), AnimSeq->RefFrameIndex);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ── Skeleton Socket helpers ────────────────────────────────────────────

static USkeleton* LoadSkeletonFromParam(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutError)
{
    FString SkeletonPath;
    if (!Params->TryGetStringField(TEXT("skeleton_path"), SkeletonPath))
    {
        OutError = FMCPCommonUtils::CreateErrorResponse(
        TEXT("Missing 'skeleton_path' parameter"),
        EMCPErrorCode::InvalidArgument,
        TEXT("`skeleton_path` is required (string, full `/Game/...` asset path to a USkeleton). Use `list_skeletons` to discover existing skeletons."));
        return nullptr;
    }
    USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
    if (!Skeleton)
    {
        OutError = FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not load skeleton: %s"), *SkeletonPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`skeleton_path` did not resolve to a USkeleton via LoadObject. Verify with `list_skeletons`. Paths are case-sensitive."));
        return nullptr;
    }
    return Skeleton;
}

static TSharedPtr<FJsonObject> SocketToJson(const USkeletalMeshSocket* Socket, int32 Index)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("index"), Index);
    Obj->SetStringField(TEXT("socket_name"), Socket->SocketName.ToString());
    Obj->SetStringField(TEXT("bone_name"), Socket->BoneName.ToString());
    Obj->SetNumberField(TEXT("location_x"), Socket->RelativeLocation.X);
    Obj->SetNumberField(TEXT("location_y"), Socket->RelativeLocation.Y);
    Obj->SetNumberField(TEXT("location_z"), Socket->RelativeLocation.Z);
    Obj->SetNumberField(TEXT("rotation_pitch"), Socket->RelativeRotation.Pitch);
    Obj->SetNumberField(TEXT("rotation_yaw"), Socket->RelativeRotation.Yaw);
    Obj->SetNumberField(TEXT("rotation_roll"), Socket->RelativeRotation.Roll);
    Obj->SetNumberField(TEXT("scale_x"), Socket->RelativeScale.X);
    Obj->SetNumberField(TEXT("scale_y"), Socket->RelativeScale.Y);
    Obj->SetNumberField(TEXT("scale_z"), Socket->RelativeScale.Z);
    Obj->SetBoolField(TEXT("force_always_animated"), Socket->bForceAlwaysAnimated);
    return Obj;
}

// ── list_skeleton_sockets ──────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleListSkeletonSockets(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    USkeleton* Skeleton = LoadSkeletonFromParam(Params, Error);
    if (!Skeleton) return Error;

    TArray<TSharedPtr<FJsonValue>> SocketArray;
    for (int32 i = 0; i < Skeleton->Sockets.Num(); ++i)
    {
        if (Skeleton->Sockets[i])
        {
            SocketArray.Add(MakeShared<FJsonValueObject>(SocketToJson(Skeleton->Sockets[i], i)));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
    ResultObj->SetArrayField(TEXT("sockets"), SocketArray);
    ResultObj->SetNumberField(TEXT("count"), SocketArray.Num());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ── add_skeleton_socket ────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleAddSkeletonSocket(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    USkeleton* Skeleton = LoadSkeletonFromParam(Params, Error);
    if (!Skeleton) return Error;

    FString SocketName;
    if (!Params->TryGetStringField(TEXT("socket_name"), SocketName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'socket_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`socket_name` is required (string, the FName of a socket on the target USkeleton). Use `list_skeleton_sockets` to enumerate existing sockets."));
    }

    FString BoneName;
    if (!Params->TryGetStringField(TEXT("bone_name"), BoneName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'bone_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`bone_name` is required (string, the FName of a bone in the target USkeleton's reference skeleton). Use `inspect_skeletal_mesh` on a mesh bound to this skeleton, or list_skeleton_sockets-style introspection to discover. Names are case-sensitive."));
    }

    // Check for duplicate socket name
    if (Skeleton->FindSocket(FName(*SocketName)))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Socket '%s' already exists on this skeleton"), *SocketName),
            EMCPErrorCode::NameCollision,
            TEXT("A socket with the requested `socket_name` already exists on this USkeleton. Pick a different name, or call `modify_skeleton_socket` to update the existing one, or `remove_skeleton_socket` first if you intend to replace it."));
    }

    // Parse transform components up-front so dry-run and apply share the same
    // input parsing. Defaults: location/rotation = identity, scale = unit.
    double X = 0, Y = 0, Z = 0;
    Params->TryGetNumberField(TEXT("location_x"), X);
    Params->TryGetNumberField(TEXT("location_y"), Y);
    Params->TryGetNumberField(TEXT("location_z"), Z);

    double Pitch = 0, Yaw = 0, Roll = 0;
    Params->TryGetNumberField(TEXT("rotation_pitch"), Pitch);
    Params->TryGetNumberField(TEXT("rotation_yaw"), Yaw);
    Params->TryGetNumberField(TEXT("rotation_roll"), Roll);

    double SX = 1, SY = 1, SZ = 1;
    Params->TryGetNumberField(TEXT("scale_x"), SX);
    Params->TryGetNumberField(TEXT("scale_y"), SY);
    Params->TryGetNumberField(TEXT("scale_z"), SZ);

    // dry_run: every preflight ran (skeleton load, socket_name + bone_name
    // fields, duplicate socket name check via FindSocket). Skip NewObject +
    // Sockets.Add. Diff shape per todo/13 phase 4: sockets_added[] with the
    // would-be index (Sockets.Add appends → would_be = current Num()), the
    // resolved skeleton path, the validated socket_name, and the parsed
    // transform components grouped as location/rotation/scale sub-objects so
    // the diff reads as a single transform rather than 9 scattered floats.
    // Note: bone_name validation (whether the bone actually exists on the
    // skeleton) is deferred to apply time — apply doesn't validate either, so
    // dry-run preserves that parity (a bone-name typo at apply time leaves the
    // socket in place with a dangling BoneName). #DEFERRED note: tracking the
    // bone-existence validation gap as a separate doc-13 follow-up.
    if (FMCPCommonUtils::ParseDryRun(Params))
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
        Entry->SetStringField(TEXT("socket_name"), SocketName);
        Entry->SetStringField(TEXT("bone_name"), BoneName);
        Entry->SetNumberField(TEXT("would_be_index"), Skeleton->Sockets.Num());

        TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
        Loc->SetNumberField(TEXT("x"), X);
        Loc->SetNumberField(TEXT("y"), Y);
        Loc->SetNumberField(TEXT("z"), Z);
        Entry->SetObjectField(TEXT("location"), Loc);

        TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
        Rot->SetNumberField(TEXT("pitch"), Pitch);
        Rot->SetNumberField(TEXT("yaw"), Yaw);
        Rot->SetNumberField(TEXT("roll"), Roll);
        Entry->SetObjectField(TEXT("rotation"), Rot);

        TSharedPtr<FJsonObject> Scale = MakeShared<FJsonObject>();
        Scale->SetNumberField(TEXT("x"), SX);
        Scale->SetNumberField(TEXT("y"), SY);
        Scale->SetNumberField(TEXT("z"), SZ);
        Entry->SetObjectField(TEXT("scale"), Scale);

        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
        TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
        Diff->SetArrayField(TEXT("sockets_added"), Arr);
        return FMCPCommonUtils::CreateDryRunResponse(Diff);
    }

    USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Skeleton);
    Socket->SocketName = FName(*SocketName);
    Socket->BoneName = FName(*BoneName);
    Socket->RelativeLocation = FVector(X, Y, Z);
    Socket->RelativeRotation = FRotator(Pitch, Yaw, Roll);
    Socket->RelativeScale = FVector(SX, SY, SZ);

    Skeleton->PreEditChange(nullptr);
    Skeleton->Sockets.Add(Socket);
    Skeleton->PostEditChange();
    Skeleton->MarkPackageDirty();

    if (!UEditorAssetLibrary::SaveAsset(Skeleton->GetPathName(), /*bOnlyIfIsDirty=*/false))
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Socket added in-memory but failed to persist to disk: %s"), *Skeleton->GetPathName()),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not written. SaveAsset no-ops while PIE is active or when the package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the change exists in-memory but will be lost on editor restart."));

    TSharedPtr<FJsonObject> ResultObj = SocketToJson(Socket, Skeleton->Sockets.Num() - 1);
    ResultObj->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ── modify_skeleton_socket ─────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleModifySkeletonSocket(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    USkeleton* Skeleton = LoadSkeletonFromParam(Params, Error);
    if (!Skeleton) return Error;

    FString SocketName;
    if (!Params->TryGetStringField(TEXT("socket_name"), SocketName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'socket_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`socket_name` is required (string, the FName of a socket on the target USkeleton). Use `list_skeleton_sockets` to enumerate existing sockets."));
    }

    USkeletalMeshSocket* Socket = Skeleton->FindSocket(FName(*SocketName));
    if (!Socket)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Socket '%s' not found"), *SocketName),
            EMCPErrorCode::NodeNotFound,
            TEXT("`socket_name` did not match any socket on the USkeleton via FindSocket. Socket names are case-sensitive FNames. Use `list_skeleton_sockets` to enumerate existing sockets."));
    }

    // Update only provided fields
    Skeleton->PreEditChange(nullptr);

    FString NewBone;
    if (Params->TryGetStringField(TEXT("bone_name"), NewBone))
    {
        Socket->BoneName = FName(*NewBone);
    }

    double Val;
    if (Params->TryGetNumberField(TEXT("location_x"), Val)) Socket->RelativeLocation.X = Val;
    if (Params->TryGetNumberField(TEXT("location_y"), Val)) Socket->RelativeLocation.Y = Val;
    if (Params->TryGetNumberField(TEXT("location_z"), Val)) Socket->RelativeLocation.Z = Val;
    if (Params->TryGetNumberField(TEXT("rotation_pitch"), Val)) Socket->RelativeRotation.Pitch = Val;
    if (Params->TryGetNumberField(TEXT("rotation_yaw"), Val)) Socket->RelativeRotation.Yaw = Val;
    if (Params->TryGetNumberField(TEXT("rotation_roll"), Val)) Socket->RelativeRotation.Roll = Val;
    if (Params->TryGetNumberField(TEXT("scale_x"), Val)) Socket->RelativeScale.X = Val;
    if (Params->TryGetNumberField(TEXT("scale_y"), Val)) Socket->RelativeScale.Y = Val;
    if (Params->TryGetNumberField(TEXT("scale_z"), Val)) Socket->RelativeScale.Z = Val;

    bool bForceAnim;
    if (Params->TryGetBoolField(TEXT("force_always_animated"), bForceAnim))
    {
        Socket->bForceAlwaysAnimated = bForceAnim;
    }

    Skeleton->PostEditChange();
    Skeleton->MarkPackageDirty();

    if (!UEditorAssetLibrary::SaveAsset(Skeleton->GetPathName(), /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Socket '%s' mutated in-memory but failed to persist to disk: %s"), *SocketName, *Skeleton->GetPathName()),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not written. SaveAsset no-ops while PIE is active or when the package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the change exists in-memory but will be lost on editor restart."));
    }

    int32 Idx = Skeleton->Sockets.IndexOfByKey(Socket);
    TSharedPtr<FJsonObject> ResultObj = SocketToJson(Socket, Idx);
    ResultObj->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ── remove_skeleton_socket ─────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleRemoveSkeletonSocket(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    USkeleton* Skeleton = LoadSkeletonFromParam(Params, Error);
    if (!Skeleton) return Error;

    FString SocketName;
    if (!Params->TryGetStringField(TEXT("socket_name"), SocketName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'socket_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`socket_name` is required (string, the FName of a socket on the target USkeleton). Use `list_skeleton_sockets` to enumerate existing sockets."));
    }

    int32 Idx;
    USkeletalMeshSocket* Socket = Skeleton->FindSocketAndIndex(FName(*SocketName), Idx);
    if (!Socket)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Socket '%s' not found"), *SocketName),
            EMCPErrorCode::NodeNotFound,
            TEXT("`socket_name` did not match any socket on the USkeleton via FindSocket. Socket names are case-sensitive FNames. Use `list_skeleton_sockets` to enumerate existing sockets."));
    }

    Skeleton->PreEditChange(nullptr);
    Skeleton->Sockets.RemoveAt(Idx);
    Skeleton->PostEditChange();
    Skeleton->MarkPackageDirty();

    if (!UEditorAssetLibrary::SaveAsset(Skeleton->GetPathName(), /*bOnlyIfIsDirty=*/false))
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Socket removed in-memory but failed to persist to disk: %s"), *Skeleton->GetPathName()),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not written. SaveAsset no-ops while PIE is active or when the package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the change exists in-memory but will be lost on editor restart."));

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
    ResultObj->SetStringField(TEXT("removed_socket"), SocketName);
    ResultObj->SetNumberField(TEXT("remaining_sockets"), Skeleton->Sockets.Num());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ═══════════════════════════════════════════════════════════════════════
// Phase 1.3: list_node_pins — lightweight pin introspection
// ═══════════════════════════════════════════════════════════════════════

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleListNodePins(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required (string). Accepts either a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints."));
    }

    FString NodeId;
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'node_id' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`node_id` is required (string, the UEdGraphNode's NodeGuid as a hex string or its short identifier). Use `analyze_blueprint_graph` or `list_blueprint_graphs` + node-walking to enumerate node IDs in the target graph."));
    }

    UBlueprint* BP = FMCPCommonUtils::FindBlueprintByName(BlueprintName);
    if (!BP)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_name` did not resolve to a Blueprint via the usual lookup (short name against `/Game/Blueprints/`, then full asset path). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints. Names are case-sensitive."));
    }

    // Determine target graph — from_state/to_state disambiguate transitions.
    // GAP-044: accept `function_name` as an alias for `graph_name` (the sibling bp_*
    // node tools scope by `function_name`); without it, list_node_pins could only be
    // scoped via `graph_name` and otherwise fell back to a cross-graph search that
    // matched a same-id node in the wrong graph.
    FString GraphName;
    FString TransFromState, TransToState;
    Params->TryGetStringField(TEXT("from_state"), TransFromState);
    Params->TryGetStringField(TEXT("to_state"), TransToState);
    if (!Params->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty())
    {
        Params->TryGetStringField(TEXT("function_name"), GraphName);
    }

    UEdGraph* Graph = nullptr;
    if (!GraphName.IsEmpty()
        || (!TransFromState.IsEmpty() && !TransToState.IsEmpty()))
    {
        Graph = FMCPCommonUtils::FindGraphByName(BP, GraphName, TransFromState, TransToState);
    }
    else
    {
        // Search all graphs
        auto SearchAll = [&](const TArray<UEdGraph*>& Graphs) -> UEdGraphNode*
        {
            for (UEdGraph* G : Graphs)
            {
                if (!G) continue;
                for (UEdGraphNode* N : G->Nodes)
                {
                    if (N->GetName() == NodeId || N->NodeGuid.ToString() == NodeId)
                    {
                        Graph = G;
                        return N;
                    }
                }
            }
            return nullptr;
        };
        SearchAll(BP->UbergraphPages);
        if (!Graph) SearchAll(BP->FunctionGraphs);
    }

    if (!Graph)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Graph not found"),
            EMCPErrorCode::NodeNotFound,
            TEXT("Could not locate the target UEdGraph on the Blueprint. Use `list_blueprint_graphs` to enumerate available graphs (EventGraph, function graphs, macro graphs)."));
    }

    // Find node
    UEdGraphNode* TargetNode = nullptr;
    for (UEdGraphNode* N : Graph->Nodes)
    {
        if (N->GetName() == NodeId || N->NodeGuid.ToString() == NodeId)
        {
            TargetNode = N;
            break;
        }
    }
    if (!TargetNode)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Node not found: %s"), *NodeId),
            EMCPErrorCode::NodeNotFound,
            TEXT("`node_id` did not match any UEdGraphNode in the Blueprint's graphs. Use `analyze_blueprint_graph` to enumerate node IDs (NodeGuid) for a given graph."));
    }

    // Serialize pins
    TArray<TSharedPtr<FJsonValue>> PinArray;
    for (UEdGraphPin* Pin : TargetNode->Pins)
    {
        if (!Pin || Pin->bHidden) continue;

        TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
        PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
        PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
        PinObj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());

        if (Pin->PinType.PinSubCategory != NAME_None)
        {
            PinObj->SetStringField(TEXT("sub_category"), Pin->PinType.PinSubCategory.ToString());
        }
        if (Pin->PinType.PinSubCategoryObject.IsValid())
        {
            PinObj->SetStringField(TEXT("sub_category_object"), Pin->PinType.PinSubCategoryObject->GetName());
        }

        PinObj->SetBoolField(TEXT("is_connected"), Pin->LinkedTo.Num() > 0);
        PinObj->SetNumberField(TEXT("num_connections"), Pin->LinkedTo.Num());

        if (!Pin->DefaultValue.IsEmpty())
        {
            PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
        }

        // List connections
        if (Pin->LinkedTo.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> ConnArray;
            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                if (!Linked || !Linked->GetOwningNodeUnchecked()) continue;
                TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
                ConnObj->SetStringField(TEXT("node_id"), Linked->GetOwningNode()->GetName());
                ConnObj->SetStringField(TEXT("pin_name"), Linked->PinName.ToString());
                ConnArray.Add(MakeShared<FJsonValueObject>(ConnObj));
            }
            PinObj->SetArrayField(TEXT("connections"), ConnArray);
        }

        PinArray.Add(MakeShared<FJsonValueObject>(PinObj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("node_id"), TargetNode->GetName());
    Result->SetStringField(TEXT("node_class"), TargetNode->GetClass()->GetName());
    Result->SetArrayField(TEXT("pins"), PinArray);
    return Result;
}

// ═══════════════════════════════════════════════════════════════════════
// Phase 5: Animation asset discovery
// ═══════════════════════════════════════════════════════════════════════

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleListBlendSpaces(const TSharedPtr<FJsonObject>& Params)
{
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AR = ARM.Get();

    FString SkeletonFilter;
    Params->TryGetStringField(TEXT("skeleton_path"), SkeletonFilter);
    FString PackageFilter;
    Params->TryGetStringField(TEXT("package_path"), PackageFilter);

    TArray<TSharedPtr<FJsonValue>> Items;

    // Search BlendSpace (2D) and BlendSpace1D
    for (const FName& ClassName : {UBlendSpace::StaticClass()->GetFName(), UBlendSpace1D::StaticClass()->GetFName()})
    {
        TArray<FAssetData> Assets;
        AR.GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/Engine"), ClassName), Assets, true);

        for (const FAssetData& Asset : Assets)
        {
            if (!PackageFilter.IsEmpty() && !Asset.PackagePath.ToString().StartsWith(PackageFilter))
                continue;

            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
            Obj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
            Obj->SetStringField(TEXT("type"), ClassName == UBlendSpace1D::StaticClass()->GetFName() ? TEXT("1D") : TEXT("2D"));

            // Check if it's an aim offset
            if (ClassName == UBlendSpace::StaticClass()->GetFName())
            {
                UBlendSpace* BS = Cast<UBlendSpace>(Asset.GetAsset());
                if (BS && BS->IsA<UAimOffsetBlendSpace>())
                {
                    Obj->SetStringField(TEXT("type"), TEXT("AimOffset2D"));
                }
            }

            Items.Add(MakeShared<FJsonValueObject>(Obj));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetArrayField(TEXT("blend_spaces"), Items);
    Result->SetNumberField(TEXT("count"), Items.Num());
    return Result;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleReadBlendSpace(const TSharedPtr<FJsonObject>& Params)
{
    FString BSPath;
    if (!Params->TryGetStringField(TEXT("blend_space_path"), BSPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blend_space_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blend_space_path` is required (string, full `/Game/...` asset path to a UBlendSpace). Use `list_blend_spaces` to discover existing blend spaces."));
    }

    UBlendSpace* BS = LoadObject<UBlendSpace>(nullptr, *BSPath);
    if (!BS)
    {
        // Try 1D
        UBlendSpace1D* BS1D = LoadObject<UBlendSpace1D>(nullptr, *BSPath);
        if (!BS1D)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("BlendSpace not found: %s"), *BSPath),
                EMCPErrorCode::AssetNotFound,
                TEXT("`blend_space_path` did not resolve to a UBlendSpace via LoadObject. Verify with `list_blend_spaces`. Paths are case-sensitive."));
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        Result->SetStringField(TEXT("path"), BSPath);
        Result->SetStringField(TEXT("type"), TEXT("1D"));

        // X axis
        const FBlendParameter& XParam = BS1D->GetBlendParameter(0);
        TSharedPtr<FJsonObject> XAxis = MakeShared<FJsonObject>();
        XAxis->SetStringField(TEXT("name"), XParam.DisplayName);
        XAxis->SetNumberField(TEXT("min"), XParam.Min);
        XAxis->SetNumberField(TEXT("max"), XParam.Max);
        XAxis->SetNumberField(TEXT("grid_divisions"), XParam.GridNum);
        Result->SetObjectField(TEXT("x_axis"), XAxis);

        // Samples
        TArray<TSharedPtr<FJsonValue>> Samples;
        for (int32 i = 0; i < BS1D->GetNumberOfBlendSamples(); ++i)
        {
            const FBlendSample& Sample = BS1D->GetBlendSample(i);
            TSharedPtr<FJsonObject> SObj = MakeShared<FJsonObject>();
            SObj->SetNumberField(TEXT("index"), i);
            SObj->SetNumberField(TEXT("x"), Sample.SampleValue.X);
            if (Sample.Animation)
            {
                SObj->SetStringField(TEXT("animation"), Sample.Animation->GetPathName());
                SObj->SetStringField(TEXT("animation_name"), Sample.Animation->GetName());
            }
            Samples.Add(MakeShared<FJsonValueObject>(SObj));
        }
        Result->SetArrayField(TEXT("samples"), Samples);
        Result->SetNumberField(TEXT("sample_count"), Samples.Num());
        return Result;
    }

    // 2D blend space
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("path"), BSPath);
    Result->SetStringField(TEXT("type"), BS->IsA<UAimOffsetBlendSpace>() ? TEXT("AimOffset2D") : TEXT("2D"));

    // Axes
    for (int32 Axis = 0; Axis < 2; ++Axis)
    {
        const FBlendParameter& Param = BS->GetBlendParameter(Axis);
        TSharedPtr<FJsonObject> AxisObj = MakeShared<FJsonObject>();
        AxisObj->SetStringField(TEXT("name"), Param.DisplayName);
        AxisObj->SetNumberField(TEXT("min"), Param.Min);
        AxisObj->SetNumberField(TEXT("max"), Param.Max);
        AxisObj->SetNumberField(TEXT("grid_divisions"), Param.GridNum);
        Result->SetObjectField(Axis == 0 ? TEXT("x_axis") : TEXT("y_axis"), AxisObj);
    }

    // Samples
    TArray<TSharedPtr<FJsonValue>> Samples;
    for (int32 i = 0; i < BS->GetNumberOfBlendSamples(); ++i)
    {
        const FBlendSample& Sample = BS->GetBlendSample(i);
        TSharedPtr<FJsonObject> SObj = MakeShared<FJsonObject>();
        SObj->SetNumberField(TEXT("index"), i);
        SObj->SetNumberField(TEXT("x"), Sample.SampleValue.X);
        SObj->SetNumberField(TEXT("y"), Sample.SampleValue.Y);
        if (Sample.Animation)
        {
            SObj->SetStringField(TEXT("animation"), Sample.Animation->GetPathName());
            SObj->SetStringField(TEXT("animation_name"), Sample.Animation->GetName());
        }
        Samples.Add(MakeShared<FJsonValueObject>(SObj));
    }
    Result->SetArrayField(TEXT("samples"), Samples);
    Result->SetNumberField(TEXT("sample_count"), Samples.Num());
    return Result;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleListAnimBlueprints(const TSharedPtr<FJsonObject>& Params)
{
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AR = ARM.Get();

    FString SkeletonFilter;
    Params->TryGetStringField(TEXT("skeleton_path"), SkeletonFilter);

    TArray<FAssetData> Assets;
    AR.GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("AnimBlueprint")), Assets, true);

    TArray<TSharedPtr<FJsonValue>> Items;
    for (const FAssetData& Asset : Assets)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        Obj->SetStringField(TEXT("path"), Asset.GetObjectPathString());

        FString ParentClass;
        if (Asset.GetTagValue(TEXT("ParentClass"), ParentClass))
        {
            Obj->SetStringField(TEXT("parent_class"), ParentClass);
        }
        FString TargetSkeleton;
        if (Asset.GetTagValue(TEXT("TargetSkeleton"), TargetSkeleton))
        {
            Obj->SetStringField(TEXT("target_skeleton"), TargetSkeleton);
            if (!SkeletonFilter.IsEmpty() && !TargetSkeleton.Contains(SkeletonFilter))
                continue;
        }

        Items.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetArrayField(TEXT("anim_blueprints"), Items);
    Result->SetNumberField(TEXT("count"), Items.Num());
    return Result;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleListAnimMontages(const TSharedPtr<FJsonObject>& Params)
{
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AR = ARM.Get();

    FString SkeletonFilter;
    Params->TryGetStringField(TEXT("skeleton_path"), SkeletonFilter);

    TArray<FAssetData> Assets;
    AR.GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("AnimMontage")), Assets, true);

    TArray<TSharedPtr<FJsonValue>> Items;
    for (const FAssetData& Asset : Assets)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        Obj->SetStringField(TEXT("path"), Asset.GetObjectPathString());

        UAnimMontage* Montage = Cast<UAnimMontage>(Asset.GetAsset());
        if (Montage)
        {
            Obj->SetNumberField(TEXT("duration"), Montage->GetPlayLength());
            if (Montage->SlotAnimTracks.Num() > 0)
            {
                Obj->SetStringField(TEXT("slot_name"), Montage->SlotAnimTracks[0].SlotName.ToString());
            }
            if (Montage->GetSkeleton())
            {
                FString SkelPath = Montage->GetSkeleton()->GetPathName();
                Obj->SetStringField(TEXT("skeleton"), SkelPath);
                if (!SkeletonFilter.IsEmpty() && !SkelPath.Contains(SkeletonFilter))
                    continue;
            }
        }

        Items.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetArrayField(TEXT("montages"), Items);
    Result->SetNumberField(TEXT("count"), Items.Num());
    return Result;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleListAnimLayerInterfaces(const TSharedPtr<FJsonObject>& Params)
{
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AR = ARM.Get();

    // AnimLayerInterface assets are Blueprints with UAnimLayerInterface parent
    TArray<FAssetData> Assets;
    AR.GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("AnimLayerInterface")), Assets, true);

    // Also search for Blueprint assets that derive from AnimLayerInterface
    if (Assets.Num() == 0)
    {
        AR.GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Blueprint")), Assets, true);
    }

    TArray<TSharedPtr<FJsonValue>> Items;
    for (const FAssetData& Asset : Assets)
    {
        FString ParentClass;
        if (Asset.GetTagValue(TEXT("ParentClass"), ParentClass))
        {
            if (!ParentClass.Contains(TEXT("AnimLayerInterface")))
                continue;
        }

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        Obj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
        Items.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetArrayField(TEXT("layer_interfaces"), Items);
    Result->SetNumberField(TEXT("count"), Items.Num());
    return Result;
}

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleReadAnimMontage(const TSharedPtr<FJsonObject>& Params)
{
    FString MontagePath;
    if (!Params->TryGetStringField(TEXT("montage_path"), MontagePath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'montage_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`montage_path` is required (string, full `/Game/...` asset path to a UAnimMontage). Use `list_anim_montages` to discover."));
    }

    UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *MontagePath);
    if (!Montage)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Montage not found: %s"), *MontagePath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`montage_path` did not resolve to a UAnimMontage via LoadObject. Verify the path with `list_anim_montages`. Paths are case-sensitive."));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("path"), MontagePath);
    Result->SetStringField(TEXT("name"), Montage->GetName());
    Result->SetNumberField(TEXT("duration"), Montage->GetPlayLength());

    if (Montage->GetSkeleton())
    {
        Result->SetStringField(TEXT("skeleton"), Montage->GetSkeleton()->GetPathName());
    }

    // Slot tracks
    TArray<TSharedPtr<FJsonValue>> SlotArray;
    for (const FSlotAnimationTrack& SlotTrack : Montage->SlotAnimTracks)
    {
        TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
        SlotObj->SetStringField(TEXT("slot_name"), SlotTrack.SlotName.ToString());
        SlotArray.Add(MakeShared<FJsonValueObject>(SlotObj));
    }
    Result->SetArrayField(TEXT("slot_tracks"), SlotArray);

    // Composite sections
    TArray<TSharedPtr<FJsonValue>> SectionArray;
    for (int32 i = 0; i < Montage->CompositeSections.Num(); ++i)
    {
        const FCompositeSection& Section = Montage->CompositeSections[i];
        TSharedPtr<FJsonObject> SecObj = MakeShared<FJsonObject>();
        SecObj->SetStringField(TEXT("name"), Section.SectionName.ToString());
        SecObj->SetNumberField(TEXT("start_time"), Section.GetTime());
        SecObj->SetNumberField(TEXT("index"), i);

        if (Section.NextSectionName != NAME_None)
        {
            SecObj->SetStringField(TEXT("next_section"), Section.NextSectionName.ToString());
        }
        SectionArray.Add(MakeShared<FJsonValueObject>(SecObj));
    }
    Result->SetArrayField(TEXT("sections"), SectionArray);

    // Blend settings
    Result->SetNumberField(TEXT("blend_in_time"), Montage->BlendIn.GetBlendTime());
    Result->SetNumberField(TEXT("blend_out_time"), Montage->BlendOut.GetBlendTime());
    Result->SetNumberField(TEXT("blend_out_trigger_time"), Montage->BlendOutTriggerTime);

    // Notifies
    TArray<TSharedPtr<FJsonValue>> NotifyArray;
    for (int32 i = 0; i < Montage->Notifies.Num(); ++i)
    {
        const FAnimNotifyEvent& Notify = Montage->Notifies[i];
        TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
        NObj->SetNumberField(TEXT("index"), i);
        NObj->SetNumberField(TEXT("trigger_time"), Notify.GetTriggerTime());
        NObj->SetNumberField(TEXT("duration"), Notify.GetDuration());
        NObj->SetStringField(TEXT("notify_name"), Notify.NotifyName.ToString());
        NObj->SetNumberField(TEXT("track_index"), Notify.TrackIndex);

        if (Notify.Notify)
        {
            NObj->SetStringField(TEXT("notify_class"), Notify.Notify->GetClass()->GetName());
        }
        else if (Notify.NotifyStateClass)
        {
            NObj->SetStringField(TEXT("notify_state_class"), Notify.NotifyStateClass->GetClass()->GetName());
            NObj->SetBoolField(TEXT("is_state"), true);
        }

        NotifyArray.Add(MakeShared<FJsonValueObject>(NObj));
    }
    Result->SetArrayField(TEXT("notifies"), NotifyArray);
    Result->SetNumberField(TEXT("notify_count"), NotifyArray.Num());

    return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
// HandleGetClassProperties — reflection-driven UPROPERTY enumeration
//
// Bridges the gap between get_blueprint_variable_details (BP-declared vars only)
// and inherited properties declared on a C++ parent UClass. Used by the Neo4j
// ingest pipeline to resolve dangling `var::*` references on AnimGraphs whose
// AnimInstance parent class is C++ (e.g. UMyCharacterAnimInstance,
// UForestAnimalAnimInstance — their UPROPERTYs aren't in the BP's NewVariables
// list, so the variable refs in graph data otherwise have no Variable node to
// point at).
//
// Inputs (params):
//   class_name           string — accepts:
//                          - fully-qualified path: "/Script/MyGame.MyAnimInstance"
//                          - short name with U-prefix: "UForestAnimalAnimInstance"
//                          - short name without prefix: "ForestAnimalAnimInstance"
//   include_inherited    bool, default false — walk Super chain
//   property_name        string, optional — return only this one property
//
// Output:
//   { class_name, class_path, parent_class, include_inherited,
//     properties: [ { name, type, sub_category_object?, category, tooltip,
//                     default_value, is_blueprint_visible, is_blueprint_writable,
//                     is_editable, is_replicated, declared_in } ],
//     property_count, success }
// ─────────────────────────────────────────────────────────────────────────────
TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleGetClassProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassName;
    if (!Params->TryGetStringField(TEXT("class_name"), ClassName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'class_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`class_name` is required (string, UClass name or FQN path). Accepted forms: short name (`Actor`), U/A-prefixed (`AActor`), or `/Script/Module.Class`."));
    }

    bool bIncludeInherited = false;
    Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);

    FString PropertyName;
    const bool bSpecificProperty = Params->TryGetStringField(TEXT("property_name"), PropertyName);

    // Resolve UClass — try in order: fully-qualified path, short name as-given, U-stripped short name.
    UClass* TargetClass = nullptr;
    if (ClassName.StartsWith(TEXT("/Script/")))
    {
        TargetClass = LoadClass<UObject>(nullptr, *ClassName);
    }
    if (!TargetClass)
    {
        TargetClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::ExactClass);
    }
    if (!TargetClass && ClassName.StartsWith(TEXT("U")) && ClassName.Len() > 1)
    {
        const FString StrippedName = ClassName.RightChop(1);
        TargetClass = FindFirstObject<UClass>(*StrippedName, EFindFirstObjectOptions::ExactClass);
    }
    if (!TargetClass && !ClassName.StartsWith(TEXT("U")))
    {
        // Some natively-declared classes use the engine's prefix conventions (A* for actors,
        // F* for structs — though those aren't UClasses); try with a U prefix as a fallback.
        const FString WithUPrefix = FString::Printf(TEXT("U%s"), *ClassName);
        TargetClass = FindFirstObject<UClass>(*WithUPrefix, EFindFirstObjectOptions::ExactClass);
    }

    if (!TargetClass)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class not found: %s"), *ClassName),
            EMCPErrorCode::ClassNotLoaded,
            TEXT("`class_name` did not resolve via FindFirstObject<UClass>. Accepts short name (`Pawn`), U/A-prefixed (`APawn`), or FQN path (`/Script/Engine.Pawn`). For C++ classes, ensure the owning module is loaded. Use `get_class_properties` on a candidate to verify it exists."));
    }

    UObject* CDO = TargetClass->GetDefaultObject();
    TArray<TSharedPtr<FJsonValue>> PropertyArray;

    // Iteration: ExcludeSuper limits to this class's own UPROPERTYs; default behavior
    // (no flag) walks the super chain. (See TFieldIterator constructor — boolean param 2 is
    // bIncludeSuper, defaulting to true.)
    const EFieldIteratorFlags::SuperClassFlags SuperFlag = bIncludeInherited
        ? EFieldIteratorFlags::IncludeSuper
        : EFieldIteratorFlags::ExcludeSuper;

    for (TFieldIterator<FProperty> It(TargetClass, SuperFlag); It; ++It)
    {
        FProperty* Prop = *It;
        if (!Prop) continue;

        const FString PropNameStr = Prop->GetName();
        if (bSpecificProperty && PropNameStr != PropertyName) continue;

        TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
        PObj->SetStringField(TEXT("name"), PropNameStr);

        // GetCPPType produces the canonical C++ type — e.g. "float", "FVector",
        // "TArray", with extended portion ("<int32>") returned via the OutExtendedTypeText pointer.
        FString ExtendedType;
        FString CppType = Prop->GetCPPType(&ExtendedType, CPPF_None);
        if (!ExtendedType.IsEmpty()) CppType += ExtendedType;
        PObj->SetStringField(TEXT("type"), CppType);

        // sub_category_object: the inner UClass/UScriptStruct/UEnum path for typed properties.
        // Mirrors what BP variable details emit for struct/enum/object pin types so callers
        // can use one code path regardless of where the property was declared.
        if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
        {
            if (ObjProp->PropertyClass)
                PObj->SetStringField(TEXT("sub_category_object"), ObjProp->PropertyClass->GetPathName());
        }
        else if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
        {
            if (StructProp->Struct)
                PObj->SetStringField(TEXT("sub_category_object"), StructProp->Struct->GetPathName());
        }
        else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
        {
            if (UEnum* E = EnumProp->GetEnum())
                PObj->SetStringField(TEXT("sub_category_object"), E->GetPathName());
        }
        else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
        {
            if (UEnum* E = ByteProp->Enum)
                PObj->SetStringField(TEXT("sub_category_object"), E->GetPathName());
        }

        PObj->SetStringField(TEXT("category"), Prop->GetMetaData(TEXT("Category")));
        PObj->SetStringField(TEXT("tooltip"), Prop->GetMetaData(TEXT("Tooltip")));

        // Where the property is declared. Useful in inherited mode — tells the caller
        // which class in the chain owns this property (and lets dedup against the
        // immediate-class properties when stitching results from multiple lookups).
        if (UClass* OwnerClass = Prop->GetOwnerClass())
        {
            PObj->SetStringField(TEXT("declared_in"), OwnerClass->GetPathName());
        }

        PObj->SetBoolField(TEXT("is_blueprint_visible"), Prop->HasAnyPropertyFlags(CPF_BlueprintVisible));
        PObj->SetBoolField(TEXT("is_blueprint_writable"), !Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly));
        PObj->SetBoolField(TEXT("is_editable"), Prop->HasAnyPropertyFlags(CPF_Edit));
        PObj->SetBoolField(TEXT("is_replicated"), Prop->HasAnyPropertyFlags(CPF_Net));

        // Default value from the CDO, exported in the same text format Unreal uses on disk.
        // ExportTextItem_Direct (preferred over the deprecated ExportTextItem) writes
        // a single property's value into a string buffer.
        if (CDO)
        {
            FString DefaultStr;
            Prop->ExportTextItem_Direct(DefaultStr,
                Prop->ContainerPtrToValuePtr<void>(CDO),
                /*Defaults*/ nullptr,
                /*Parent*/ nullptr,
                PPF_None);
            PObj->SetStringField(TEXT("default_value"), DefaultStr);
        }

        PropertyArray.Add(MakeShared<FJsonValueObject>(PObj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("class_name"), TargetClass->GetName());
    ResultObj->SetStringField(TEXT("class_path"), TargetClass->GetPathName());
    if (UClass* SuperClass = TargetClass->GetSuperClass())
    {
        ResultObj->SetStringField(TEXT("parent_class"), SuperClass->GetPathName());
    }
    ResultObj->SetBoolField(TEXT("include_inherited"), bIncludeInherited);

    if (bSpecificProperty)
    {
        ResultObj->SetStringField(TEXT("property_name"), PropertyName);
        if (PropertyArray.Num() > 0)
        {
            ResultObj->SetObjectField(TEXT("property"), PropertyArray[0]->AsObject());
        }
        else
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Property not found: %s on %s"), *PropertyName, *TargetClass->GetName()),
                EMCPErrorCode::InvalidArgument,
                TEXT("`property_name` did not match any FProperty on the target class via TFieldIterator. Names are case-sensitive and must match the UPROPERTY identifier. Pass `include_inherited: true` to search the super chain. Omit `property_name` to list all properties."));
        }
    }
    else
    {
        ResultObj->SetArrayField(TEXT("properties"), PropertyArray);
        ResultObj->SetNumberField(TEXT("property_count"), PropertyArray.Num());
    }

    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}
// ── set_blueprint_default_value ─────────────────────────────────────────────
// Writes a default-value override on a Blueprint class's CDO for any
// edit-exposed UPROPERTY.  Closes the gap between
// set_blueprint_variable_properties (BP-NEW variables only) and the
// project's need to bind inherited C++ UPROPERTYs from BP — e.g. the
// new `WarmupProfile` field declared on the project's game-mode class that
// BP_<Level>_GameMode subclasses need to bind a UWarmupProfile asset to.
//
// Mirrors set_data_asset_property's FProperty-write pattern.  Recompiles
// + saves the blueprint on success.

TSharedPtr<FJsonObject> FMCPBlueprintCommands::HandleSetBlueprintDefaultValue(
    const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintRef;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintRef))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required (string). Accepts either a short name (resolved against `/Game/Blueprints/`) or a full asset path (`/Game/Folder/BP_Foo`). Use `list_assets` with `asset_type='Blueprint'` to discover existing Blueprints."));
    }

    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property"), PropertyName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'property' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`property` is required (string, the FProperty name on the Blueprint's CDO class). Use `get_class_properties` to enumerate properties on the GeneratedClass."));
    }

    TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(TEXT("value"));
    if (!JsonValue.IsValid())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'value' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`value` is required. Pass the new default value for the CDO property — JSON shape must match the property's CPP type (see `get_class_properties` for the expected type)."));
    }

    // Load the blueprint.  Accept either /Game/... path or short name —
    // FindObject with no outer is sufficient for paths; for short names
    // the AssetRegistry lookup happens via UEditorAssetLibrary.
    UObject* Loaded = UEditorAssetLibrary::LoadAsset(BlueprintRef);
    if (!Loaded && !BlueprintRef.Contains(TEXT(".")))
    {
        // Try with .AssetName suffix appended (mirrors LoadDataAssetByPath).
        const FString AssetName = FPaths::GetBaseFilename(BlueprintRef);
        const FString FullPath = FString::Printf(TEXT("%s.%s"), *BlueprintRef, *AssetName);
        Loaded = LoadObject<UObject>(nullptr, *FullPath);
    }
    UBlueprint* Blueprint = Cast<UBlueprint>(Loaded);
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintRef),
            EMCPErrorCode::AssetNotFound,
            TEXT("`blueprint_name` did not resolve to a UBlueprint via LoadAsset (with .AssetName suffix fallback). Verify with `list_assets` (asset_type='Blueprint'). Paths are case-sensitive."));
    }

    UClass* GenClass = Blueprint->GeneratedClass;
    if (!GenClass)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint '%s' has no GeneratedClass — uncompiled?"), *Blueprint->GetName()),
            EMCPErrorCode::AssetCompileFailed,
            TEXT("UBlueprint::GeneratedClass is null. The Blueprint has not been compiled (or last compile failed). Call `compile_blueprint` first, then retry. Check the editor Output Log for compile errors."));
    }

    UObject* CDO = GenClass->GetDefaultObject();
    if (!CDO)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("GeneratedClass has no CDO"),
            EMCPErrorCode::Internal,
            TEXT("UBlueprint::GeneratedClass->GetDefaultObject() returned null. The Blueprint may not have been compiled — call `compile_blueprint` first. If compilation succeeds and this persists, the Blueprint is in a corrupted state; restart the editor."));
    }

    // GAP-011: a dotted property whose head names an SCS component
    // (e.g. "Floor.RelativeScale3D") targets a component TEMPLATE, not a CDO
    // FProperty — FindPropertyByName would fail. Delegate to the same
    // component-template setter as bp_set_component_property (resolves the SCS
    // node, then walks the dotted sub-path into structs/sub-objects).
    {
        int32 DotIdx = INDEX_NONE;
        if (PropertyName.FindChar(TEXT('.'), DotIdx) && Blueprint->SimpleConstructionScript)
        {
            const FString HeadName = PropertyName.Left(DotIdx);
            const FString SubPath  = PropertyName.RightChop(DotIdx + 1);
            for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
            {
                if (Node && Node->ComponentTemplate &&
                    Node->GetVariableName().ToString() == HeadName)
                {
                    FString WriteError;
                    if (!FMCPCommonUtils::SetObjectPropertyByPath(Node->ComponentTemplate, SubPath, JsonValue, WriteError))
                    {
                        return FMCPCommonUtils::CreateErrorResponse(
                            FString::Printf(TEXT("Failed to set %s: %s"), *PropertyName, *WriteError),
                            EMCPErrorCode::InvalidArgument,
                            TEXT("Dotted component sub-path failed. Verify the component variable name (head) and the sub-property path. You can also use bp_set_component_property directly."));
                    }
                    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
                    Blueprint->MarkPackageDirty();
                    UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false);

                    TSharedPtr<FJsonObject> CompResult = MakeShared<FJsonObject>();
                    CompResult->SetBoolField(TEXT("success"), true);
                    CompResult->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
                    CompResult->SetStringField(TEXT("property"), PropertyName);
                    CompResult->SetStringField(TEXT("component"), HeadName);
                    return CompResult;
                }
            }
        }
    }

    FProperty* Property = GenClass->FindPropertyByName(FName(*PropertyName));
    if (!Property)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *GenClass->GetName()),
            EMCPErrorCode::InvalidArgument,
            TEXT("`property` did not match any FProperty on the Blueprint's GeneratedClass. Names are case-sensitive UPROPERTY identifiers. For a component sub-property use the dotted form `Component.Property` or bp_set_component_property. Use `get_class_properties` on the GeneratedClass to enumerate."));
    }

    // Reject writes to non-edit-exposed properties (matches set_data_asset_property).
    const uint64 RequiredAny = CPF_Edit | CPF_BlueprintAssignable | CPF_BlueprintVisible;
    if ((Property->PropertyFlags & RequiredAny) == 0)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Property '%s' is not edit-exposed (no CPF_Edit / CPF_BlueprintVisible)"), *PropertyName),
            EMCPErrorCode::InvalidArgument,
            TEXT("The named property exists but lacks EditAnywhere / EditDefaultsOnly / BlueprintReadWrite / BlueprintAssignable / BlueprintVisible — external mutation is not permitted. Modify the property's UPROPERTY specifier in C++ to expose it for default-value writes, or pick a different property."));
    }

    // Bracket the write with PreEditChange / PostEditChangeProperty.  UE's
    // property change pipeline relies on these to (a) mark the CDO dirty
    // for undo and serialization, (b) flag the owning Blueprint as
    // modified, and (c) propagate the override delta to subclasses.
    // Skipping them is why a bare JsonValueToUProperty + SaveAsset writes
    // the live CDO but the saved .uasset doesn't carry the change —
    // observed 2026-05-06 when binding WarmupProfile on BP_<GameMode>.
    Blueprint->Modify();
    CDO->Modify();
    CDO->PreEditChange(Property);

    void* PropertyData = Property->ContainerPtrToValuePtr<void>(CDO);
    if (!FJsonObjectConverter::JsonValueToUProperty(JsonValue, Property, PropertyData, 0, 0))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to convert value for property '%s' (type=%s)"), *PropertyName, *Property->GetCPPType()),
            EMCPErrorCode::InvalidArgument,
            TEXT("`value` did not convert through FJsonObjectConverter::JsonValueToUProperty against the property's CPP type. The error message lists the expected type — match it with the correct JSON shape: structs are JSON objects with field names matching UPROPERTY identifiers, enums are JSON strings of the enum-element name, object refs are JSON strings of the asset path, soft refs and TSubclassOf are asset paths too."));
    }

    FPropertyChangedEvent ChangeEvent(Property, EPropertyChangeType::ValueSet);
    CDO->PostEditChangeProperty(ChangeEvent);

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    Blueprint->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(Blueprint->GetPathName(), /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Set %s.%s in memory but saving the blueprint to disk failed (UEditorAssetLibrary::SaveAsset returned false)"), *Blueprint->GetName(), *PropertyName),
            EMCPErrorCode::Internal,
            TEXT("The CDO default was written and the blueprint marked dirty, but the save-to-disk call returned false — the change was NOT persisted and will be lost on editor restart. Common causes: -unattended editor (EditorAssetSubsystem save paths no-op), read-only / un-checked-out file, or package validation failure. Save manually, or re-run interactively."));
    }

    UE_LOG(LogUnrealMCP, Display,
        TEXT("set_blueprint_default_value: %s.%s ← <value>"),
        *Blueprint->GetName(), *PropertyName);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
    Result->SetStringField(TEXT("property"), PropertyName);
    return Result;
}
