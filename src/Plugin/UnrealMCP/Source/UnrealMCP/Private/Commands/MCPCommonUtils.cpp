#include "Commands/MCPCommonUtils.h"
#include "JsonObjectConverter.h"

DEFINE_LOG_CATEGORY(LogUnrealMCP);

#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "GameFramework/Actor.h"
#include "Engine/Blueprint.h"
#include "Materials/Material.h"
#include "MaterialShared.h"
#include "EdGraph/EdGraph.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Components/StaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/UObjectIterator.h"
#include "Engine/Selection.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Misc/PackageName.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintActionDatabase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// JSON Utilities
TSharedPtr<FJsonObject> FMCPCommonUtils::CreateErrorResponse(const FString& Message)
{
    TSharedPtr<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
    ResponseObject->SetBoolField(TEXT("success"), false);
    ResponseObject->SetStringField(TEXT("error"), Message);
    return ResponseObject;
}

TSharedPtr<FJsonObject> FMCPCommonUtils::CreateErrorResponse(const FString& Message, EMCPErrorCode Code, const FString& Hint)
{
    TSharedPtr<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
    ResponseObject->SetBoolField(TEXT("success"), false);
    ResponseObject->SetStringField(TEXT("error"), Message);
    ResponseObject->SetStringField(TEXT("error_code"), MCPErrorCodeToString(Code));
    if (!Hint.IsEmpty())
    {
        ResponseObject->SetStringField(TEXT("error_hint"), Hint);
    }
    return ResponseObject;
}

bool FMCPCommonUtils::ParseDryRun(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return false;
    }
    bool bDryRun = false;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
    return bDryRun;
}

TSharedPtr<FJsonObject> FMCPCommonUtils::CreateDryRunResponse(const TSharedPtr<FJsonObject>& Diff)
{
    TSharedPtr<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
    ResponseObject->SetBoolField(TEXT("success"), true);
    ResponseObject->SetBoolField(TEXT("dry_run"), true);
    if (Diff.IsValid())
    {
        ResponseObject->SetObjectField(TEXT("diff"), Diff);
    }
    return ResponseObject;
}

TSharedPtr<FJsonObject> FMCPCommonUtils::CreateDryRunUnsupportedResponse(const FString& ToolName)
{
    return CreateErrorResponse(
        FString::Printf(TEXT("Tool '%s' does not yet support dry_run"), *ToolName),
        EMCPErrorCode::DryRunUnsupported,
        TEXT("Re-run without dry_run, or wait for handler-side support to land. See mcp/docs/todo/13_dry_run_plumbing.md for the rollout phase order."));
}

bool FMCPCommonUtils::IsBlockedFromDryRun(const FString& CommandType)
{
    // Known mutators that DON'T support dry_run yet. The bridge intercepts these
    // and returns CreateDryRunUnsupportedResponse, giving agents a clear error
    // signal instead of silently dispatching with dry_run ignored.
    //
    // Removing an entry from this set means the mutator has shipped handler-side
    // dry_run support — the entry must be removed in the same commit so the
    // safety net doesn't shadow the real implementation.
    //
    // Audit basis: a mutator is in this set iff its handler file contains no
    // `ParseDryRun(Params)` call. The audit covered every command-handling .cpp
    // under Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/ as of iter 26.
    // Read tools and dry_run-supporting mutators are intentionally absent.
    static const TSet<FString> BlockedCommands = {
        // ── doc-13 Phase 2 BLOCKED ────────────────────────────────────────
        // Investigated 2026-05-10. Per-type dispatch tree (~67 node types)
        // is co-mingled with construction; per-type validation extraction
        // out of scope for autonomous iteration. Awaits user 3-option matrix.
        TEXT("bp_add_node"),
        TEXT("add_blueprint_node"),  // legacy alias for add_node

        // ── doc-7 Widget UMG (4 mutators, no ParseDryRun calls in file) ──
        TEXT("widget_create"),
        TEXT("widget_add_child"),
        TEXT("widget_bind_handler"),
        TEXT("widget_set_property"),

        // ── Animation retargeting — IK Rig + Retargeter authoring + batch ──
        // None of these accept dry_run (the asset-create + chain-wire mutators
        // are append/overwrite shaped — no preview semantics). Each performs an
        // atomic asset-side effect and saves before returning.
        TEXT("ik_retarget_create"),                          // creates UIKRetargeter asset
        TEXT("ik_retarget_set_rigs"),                        // mutates source/target rig refs
        TEXT("ik_retarget_auto_map_chains"),                 // bulk chain wiring
        TEXT("ik_retarget_set_chain_mapping"),               // single chain wire
        TEXT("ik_retarget_import_pose_from_pose_asset"),     // creates retarget pose from PoseAsset
        TEXT("ik_retarget_import_pose_from_animation"),      // creates retarget pose from anim sequence frame
        TEXT("ik_retarget_align_bones"),                     // reset + AutoAlignAllBones on one side
        TEXT("ik_retarget_run_batch"),                         // batch produces output anim assets
        TEXT("ik_retarget_set_pelvis_settings"),             // mutates FIKRetargetPelvisMotionOpSettings
        TEXT("ik_retarget_set_root_motion_settings"),        // mutates FIKRetargetRootMotionOpSettings
        TEXT("anim_smooth_sequence"),                          // creates/mutates a UAnimSequence with smoothed bone tracks
        TEXT("anim_normalize_z_offset"),                       // creates/mutates a UAnimSequence with frame-0 Z rebased
        TEXT("anim_anchor_feet_to_floor"),                          // FK-composes foot world Z, shifts pelvis to anchor feet at floor

        // ── doc-10 GAS asset authoring (mutators) ─────────────────────────
        // gas_effect_apply intentionally NOT listed — runtime tool, per
        // doc-13 DEFERRED ("dry-run for runtime tools, semantics don't translate").
        TEXT("gas_ability_create"),
        TEXT("gas_ability_set_cost"),
        TEXT("gas_ability_set_cooldown"),
        TEXT("gas_effect_create"),
        TEXT("gas_attributeset_create"),

        // ── Mesh / FBX import (GAP-001) ───────────────────────────────────
        // External import has no preview semantics — it either lands the asset
        // on disk or it doesn't; nothing meaningful to diff for a dry run.
        TEXT("asset_import_mesh"),

        // ── Level persistence (GAP-006) ───────────────────────────────────
        // New/save/load swap or persist the active world — no preview/diff.
        TEXT("level_new"),
        TEXT("level_save"),
        TEXT("level_save_as"),
        TEXT("level_load"),
        // Reflection-capture bake — it either bakes the cubemaps or not; no preview.
        TEXT("editor_build_reflection_captures"),

        // ── doc-6 Asset Factory (asset-creation mutators) ─────────────────
        TEXT("enum_create"),
        TEXT("struct_create"),
        TEXT("datatable_create"),
        TEXT("mpc_create"),
        TEXT("material_function_create"),
        TEXT("niagara_script_create"),
        TEXT("input_create"),
        TEXT("input_add_mapping"),
        TEXT("physics_material_create"),

        // ── PIE video recording ───────────────────────────────────────────
        // Live-session side effects (encoder thread + file on disk) — nothing
        // meaningful to diff for a dry run.
        TEXT("pie_record_start"),
        TEXT("pie_record_stop"),
        TEXT("pie_record_arm"),
        TEXT("pie_record_disarm"),
    };
    return BlockedCommands.Contains(CommandType);
}

bool FMCPCommonUtils::IsBlockedDuringPie(const FString& CommandType)
{
    // Asset mutate/load commands that MUST NOT run while a Play-In-Editor session
    // is active. During PIE, UEditorAssetLibrary::LoadAsset →
    // EditorScriptingHelpers::CheckIfInEditorAndPIE returns null (it refuses while
    // GEditor->PlayWorld is set), so every command that loads/edits/saves a
    // content asset on the game thread fails — historically with a MISLEADING
    // "asset not found" / intermittent socket error rather than a clear cause.
    // The bridge refuses these with EMCPErrorCode::PieActive while PIE runs.
    //
    // This is a deliberate BLOCKLIST: PIE-driving automation (start/stop PIE,
    // synthetic input, screenshots, console exec), AI-runtime reads, and
    // registry/world/status reads (list_assets, get_actors_in_level, mcp_status,
    // …) are intentionally ABSENT so they keep working during PIE and cannot be
    // broken by an over-eager gate. A genuine asset command missing from this set
    // simply retains its prior (pre-gate) behavior — no regression, just no clean
    // pie_active message for it yet. Names are the canonical command strings the
    // bridge dispatches on directly (there is no alias resolution step).
    static const TSet<FString> BlockedCommands = {
        // ── Asset CRUD / lifecycle ────────────────────────────────────────
        TEXT("asset_open"), TEXT("asset_save"), TEXT("asset_rename"),
        TEXT("asset_move"), TEXT("asset_duplicate"), TEXT("asset_delete"),
        TEXT("asset_fixup_redirectors"),

        // ── Blueprint (asset-loading + mutating) ──────────────────────────
        TEXT("bp_create_blueprint"), TEXT("bp_set_default_value"),
        TEXT("bp_add_component"), TEXT("bp_set_component_property"),
        TEXT("bp_set_component_transform"), TEXT("bp_set_class_replication"),
        TEXT("bp_set_event_replication"),
        TEXT("physics_set_properties"),
        TEXT("bp_compile"), TEXT("mesh_set_static_mesh_properties"),
        TEXT("mesh_set_mesh_material_color"), TEXT("material_apply_to_actor"),
        TEXT("material_apply_to_blueprint"), TEXT("mesh_set_static_mesh_material"),
        TEXT("bp_reparent"), TEXT("anim_blend_space_create"),
        TEXT("anim_blend_space_add_sample"), TEXT("anim_blend_space_remove_sample"),
        TEXT("anim_sequence_set_property"), TEXT("anim_skeleton_add_socket"),
        TEXT("anim_skeleton_modify_socket"), TEXT("anim_skeleton_remove_socket"),

        // ── Blueprint graph mutators ──────────────────────────────────────
        TEXT("add_blueprint_node"), TEXT("bp_add_node"), TEXT("bp_connect_pins"),
        TEXT("bp_create_variable"), TEXT("bp_delete_variable"),
        TEXT("bp_set_variable_properties"), TEXT("bp_add_event_node"),
        TEXT("bp_delete_node"), TEXT("bp_set_node_property"), TEXT("bp_create_function"),
        TEXT("bp_add_function_input"), TEXT("bp_add_function_output"),
        TEXT("bp_remove_function_input"), TEXT("bp_remove_function_output"),
        TEXT("bp_delete_function"), TEXT("bp_rename_function"),
        TEXT("bp_create_dispatcher"),
        TEXT("bp_remove_component"), TEXT("bp_disconnect_pin"),

        // ── Material graph ────────────────────────────────────────────────
        TEXT("material_create"), TEXT("material_set_property"), TEXT("material_create_instance"),
        TEXT("material_instance_set_parameter"), TEXT("material_reparent_instance"),
        TEXT("material_compile"), TEXT("material_add_expression"),
        TEXT("material_set_expression_property"), TEXT("material_delete_expression"),
        TEXT("material_connect"),

        // ── Texture / Mesh import / Data Asset ─────────────────────────────
        TEXT("asset_textures_import"), TEXT("asset_import_mesh"),
        TEXT("asset_dataasset_create"), TEXT("asset_dataasset_set_property"),

        // ── Level persistence (GAP-006) ───────────────────────────────────
        TEXT("level_new"), TEXT("level_save"),
        TEXT("level_save_as"), TEXT("level_load"),
        // Reflection-capture bake renders the editor scene — not valid during PIE.
        TEXT("editor_build_reflection_captures"),

        // ── Asset factory (asset creation) ────────────────────────────────
        TEXT("enum_create"), TEXT("struct_create"), TEXT("datatable_create"),
        TEXT("mpc_create"), TEXT("material_function_create"),
        TEXT("niagara_script_create"), TEXT("input_create"),
        TEXT("input_add_mapping"), TEXT("physics_material_create"),

        // ── Widget (UMG authoring) ────────────────────────────────────────
        TEXT("widget_create"), TEXT("widget_add_child"),
        TEXT("widget_bind_handler"), TEXT("widget_set_property"),

        // ── IK retarget authoring + anim-sequence bakes ───────────────────
        TEXT("ik_retarget_create"), TEXT("ik_retarget_set_rigs"),
        TEXT("ik_retarget_auto_map_chains"), TEXT("ik_retarget_set_chain_mapping"),
        TEXT("ik_retarget_import_pose_from_pose_asset"),
        TEXT("ik_retarget_import_pose_from_animation"),
        TEXT("ik_retarget_align_bones"), TEXT("ik_retarget_run_batch"),
        TEXT("ik_retarget_set_pelvis_settings"),
        TEXT("ik_retarget_set_root_motion_settings"),
        TEXT("anim_smooth_sequence"), TEXT("anim_normalize_z_offset"),
        TEXT("anim_anchor_feet_to_floor"),

        // ── Gameplay tags / GAS authoring ─────────────────────────────────
        TEXT("tag_add"), TEXT("tag_remove"), TEXT("tag_move"),
        TEXT("gas_ability_create"), TEXT("gas_ability_set_cost"),
        TEXT("gas_ability_set_cooldown"), TEXT("gas_effect_create"),
        TEXT("gas_effect_apply"), TEXT("gas_attributeset_create"),

        // ── Niagara authoring ─────────────────────────────────────────────
        TEXT("niagara_user_parameter_add"), TEXT("niagara_user_parameter_remove"),
        TEXT("niagara_user_parameter_set"), TEXT("niagara_emitter_set_enabled"),
        TEXT("niagara_module_set_input"), TEXT("niagara_scratch_pad_module_add"),
        TEXT("niagara_system_create"), TEXT("niagara_emitter_add"),
        TEXT("niagara_emitter_add_renderer"), TEXT("niagara_renderer_set_material"),
        TEXT("niagara_renderer_set_material_binding"), TEXT("niagara_module_add"),
        TEXT("niagara_emitter_set_local_space"), TEXT("niagara_renderer_set_alignment"),
        TEXT("niagara_mesh_renderer_set_mesh"), TEXT("niagara_renderer_set_enabled"),

        // ── Animation authoring ───────────────────────────────────────────
        TEXT("anim_state_machine_create"), TEXT("anim_state_machine_state_add"), TEXT("add_conduit"),
        TEXT("anim_state_machine_transition_add"), TEXT("anim_state_machine_set_entry"), TEXT("anim_state_machine_modify_transition"),
        TEXT("anim_state_machine_state_remove"), TEXT("anim_state_machine_transition_remove"), TEXT("bp_set_inner_node_property"),
        TEXT("anim_node_bind_property"), TEXT("anim_notify_add"),
        TEXT("anim_notify_remove"), TEXT("anim_extract_between_notifies"),
        TEXT("anim_montage_create"), TEXT("anim_montage_add_section"),
        TEXT("anim_montage_set_section_link"), TEXT("anim_montage_set_blend"),
        TEXT("anim_blueprint_create"), TEXT("anim_blueprint_set_skeleton"),

        // ── Skeletal mesh / physics asset (mutators + the two asset loaders
        //    named as PIE offenders in the bug log) ────────────────────────
        TEXT("anim_skeletal_mesh_inspect"), TEXT("anim_physics_inspect"),
        TEXT("anim_skeletal_mesh_set_section_disabled"), TEXT("physics_set_body_collision"),
        TEXT("physics_set_constraint_motion"), TEXT("mesh_set_physics_asset"),
        TEXT("merge_bones_into_skeleton"), TEXT("merge_bones_into_skeletal_mesh"),
        TEXT("mesh_build_bend_chain"),

        // ── Static mesh bake / collision / sockets ────────────────────────
        TEXT("asset_bake_dynamic_to_static_mesh"), TEXT("mesh_set_collision"),
        TEXT("mesh_add_socket"), TEXT("mesh_modify_socket"), TEXT("mesh_remove_socket"),

        // ── StateTree authoring (compile/save/state/node/transition mutate) ─
        TEXT("statetree_create"), TEXT("statetree_compile"), TEXT("statetree_save"),
        TEXT("st_add_state"), TEXT("st_remove_state"), TEXT("st_rename_state"),
        TEXT("st_move_state"), TEXT("st_duplicate_state"), TEXT("st_set_state_properties"),
        TEXT("st_add_node"), TEXT("st_remove_node"), TEXT("st_set_node_property"),
        TEXT("st_add_transition"), TEXT("st_remove_transition"),
        TEXT("st_set_transition_properties"), TEXT("st_add_property_binding"),
        TEXT("st_remove_property_binding"),

        // ── EQS authoring ─────────────────────────────────────────────────
        TEXT("eqs_create"), TEXT("eqs_option_add"), TEXT("eqs_test_add"),
        TEXT("eqs_option_remove"), TEXT("eqs_test_remove"), TEXT("eqs_set_property"),
    };
    return BlockedCommands.Contains(CommandType);
}

FString FMCPCommonUtils::MCPErrorCodeToString(EMCPErrorCode Code)
{
    switch (Code)
    {
    case EMCPErrorCode::AssetNotFound:           return TEXT("asset_not_found");
    case EMCPErrorCode::ClassNotLoaded:          return TEXT("class_not_loaded");
    case EMCPErrorCode::NodeNotFound:            return TEXT("node_not_found");
    case EMCPErrorCode::ActorNotFound:           return TEXT("actor_not_found");
    case EMCPErrorCode::VariableNotFound:        return TEXT("variable_not_found");
    case EMCPErrorCode::PinNotFound:             return TEXT("pin_not_found");
    case EMCPErrorCode::FunctionNotFound:        return TEXT("function_not_found");
    case EMCPErrorCode::UnknownTag:              return TEXT("unknown_tag");

    case EMCPErrorCode::InvalidArgument:         return TEXT("invalid_argument");
    case EMCPErrorCode::InvalidPath:             return TEXT("invalid_path");
    case EMCPErrorCode::InvalidPinType:          return TEXT("invalid_pin_type");
    case EMCPErrorCode::AmbiguousTarget:         return TEXT("ambiguous_target");
    case EMCPErrorCode::OutOfRange:              return TEXT("out_of_range");

    case EMCPErrorCode::AssetDirty:              return TEXT("asset_dirty");
    case EMCPErrorCode::AssetCompileFailed:      return TEXT("asset_compile_failed");
    case EMCPErrorCode::AssetLocked:             return TEXT("asset_locked");
    case EMCPErrorCode::NameCollision:           return TEXT("name_collision");

    case EMCPErrorCode::UnsupportedClass:        return TEXT("unsupported_class");
    case EMCPErrorCode::NotInPie:                return TEXT("not_in_pie");
    case EMCPErrorCode::FeatureDisabled:         return TEXT("feature_disabled");
    case EMCPErrorCode::DryRunUnsupported:       return TEXT("dry_run_unsupported");

    case EMCPErrorCode::WouldBreakReferences:    return TEXT("would_break_references");
    case EMCPErrorCode::CircularDependency:      return TEXT("circular_dependency");

    case EMCPErrorCode::EngineBusy:              return TEXT("engine_busy");
    case EMCPErrorCode::LiveCodingUnavailable:   return TEXT("live_coding_unavailable");
    case EMCPErrorCode::CompileInProgress:       return TEXT("compile_in_progress");
    case EMCPErrorCode::Internal:                return TEXT("internal");
    case EMCPErrorCode::Timeout:                 return TEXT("timeout");

    case EMCPErrorCode::WindowNotFound:          return TEXT("window_not_found");

    case EMCPErrorCode::EditorNotReady:          return TEXT("editor_not_ready");
    case EMCPErrorCode::PieActive:               return TEXT("pie_active");

    default:                                     return TEXT("internal");
    }
}

TSharedPtr<FJsonObject> FMCPCommonUtils::CreateSuccessResponse(const TSharedPtr<FJsonObject>& Data)
{
    TSharedPtr<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
    ResponseObject->SetBoolField(TEXT("success"), true);
    
    if (Data.IsValid())
    {
        ResponseObject->SetObjectField(TEXT("data"), Data);
    }

    return ResponseObject;
}

void FMCPCommonUtils::RecompileMaterialWithDependents(UMaterial* Material)
{
    if (!Material)
    {
        return;
    }

    // The FMaterialUpdateContext destructor (Engine FMaterialUpdateContext::~FMaterialUpdateContext)
    // is what walks every dependent UMaterialInstance of an updated material and rebuilds its
    // uniform-expression set / texture bindings. PostEditChange() alone recompiles only the base
    // UMaterial, leaving child instances (what meshes actually render) with a stale, textureless
    // proxy that draws as the default grey-checker. Scope the edit inside the context so the
    // destructor fires after PostEditChange has finalized the base material.
    {
        FMaterialUpdateContext UpdateContext;
        UpdateContext.AddMaterial(Material);
        Material->PostEditChange();
        Material->MarkPackageDirty();
    }
}

TSharedPtr<FJsonObject> FMCPCommonUtils::SaveMaterialOrError(UMaterial* Material)
{
    if (!Material)
    {
        return nullptr;
    }

    // GAP-062: graph mutators previously left the package dirty-but-unsaved, so
    // edits reverted on the next editor reload. Persist now, mirroring the
    // material-instance setters' SaveLoadedAsset(MIC, false) pattern. bOnlyIfIsDirty
    // is false to match the rest of the material family (the mutator already
    // MarkPackageDirty'd via RecompileMaterialWithDependents, but force the write
    // regardless so a stale dirty-flag state can't drop the save).
    if (!UEditorAssetLibrary::SaveLoadedAsset(Material, /*bOnlyIfIsDirty=*/false))
    {
        return CreateErrorResponse(
            FString::Printf(TEXT("Material mutated in-memory but failed to persist to disk: %s"), *Material->GetPathName()),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveLoadedAsset returned false — the change was NOT written to disk and will be lost on editor restart. SaveLoadedAsset no-ops while PIE is active or when the package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry."));
    }

    return nullptr;
}

void FMCPCommonUtils::GetIntArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<int32>& OutArray)
{
    OutArray.Reset();
    
    if (!JsonObject->HasField(FieldName))
    {
        return;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray))
    {
        for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
        {
            OutArray.Add((int32)Value->AsNumber());
        }
    }
}

void FMCPCommonUtils::GetFloatArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<float>& OutArray)
{
    OutArray.Reset();
    
    if (!JsonObject->HasField(FieldName))
    {
        return;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray))
    {
        for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
        {
            OutArray.Add((float)Value->AsNumber());
        }
    }
}

FVector2D FMCPCommonUtils::GetVector2DFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FVector2D Result(0.0f, 0.0f);
    
    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 2)
    {
        Result.X = (float)(*JsonArray)[0]->AsNumber();
        Result.Y = (float)(*JsonArray)[1]->AsNumber();
    }
    
    return Result;
}

FVector FMCPCommonUtils::GetVectorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FVector Result(0.0f, 0.0f, 0.0f);
    
    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 3)
    {
        Result.X = (float)(*JsonArray)[0]->AsNumber();
        Result.Y = (float)(*JsonArray)[1]->AsNumber();
        Result.Z = (float)(*JsonArray)[2]->AsNumber();
    }
    
    return Result;
}

FRotator FMCPCommonUtils::GetRotatorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FRotator Result(0.0f, 0.0f, 0.0f);
    
    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 3)
    {
        Result.Pitch = (float)(*JsonArray)[0]->AsNumber();
        Result.Yaw = (float)(*JsonArray)[1]->AsNumber();
        Result.Roll = (float)(*JsonArray)[2]->AsNumber();
    }
    
    return Result;
}

// Blueprint Utilities
UBlueprint* FMCPCommonUtils::FindBlueprint(const FString& BlueprintName)
{
    return FindBlueprintByName(BlueprintName);
}

UBlueprint* FMCPCommonUtils::FindBlueprintByName(const FString& BlueprintName)
{
    // The correct object path for a Blueprint asset is /Game/Path/AssetName.AssetName
    FString ObjectPath;

    // Check if BlueprintName is already a full path (starts with /)
    if (BlueprintName.StartsWith(TEXT("/")))
    {
        // It's already a full path, use it directly with the class suffix
        FString AssetName = FPaths::GetBaseFilename(BlueprintName);
        ObjectPath = FString::Printf(TEXT("%s.%s"), *BlueprintName, *AssetName);
    }
    else
    {
        // It's just a name, add the default /Game/Blueprints/ prefix
        ObjectPath = FString::Printf(TEXT("/Game/Blueprints/%s.%s"), *BlueprintName, *BlueprintName);
    }

    // First, try to load the object directly, as it's the fastest method.
    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
    if (Blueprint)
    {
        return Blueprint;
    }

    // If direct loading fails, try to find the asset using the Asset Registry.
    // This is more robust for newly created assets.
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    FAssetData AssetData = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));

    if (AssetData.IsValid())
    {
        Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
        if (Blueprint)
        {
            return Blueprint;
        }
    }

    // Fallback for cases where the asset is in memory but not yet fully saved,
    // where it might be found via its package path.
    FString PackagePath = TEXT("/Game/Blueprints/") + BlueprintName;
    Blueprint = FindObject<UBlueprint>(nullptr, *PackagePath);

    if (Blueprint)
    {
        return Blueprint;
    }

    // Final fallback: scan the Asset Registry by name for blueprints outside /Game/Blueprints/
    // This handles blueprints at arbitrary paths (e.g. /Game/Characters/BP_Horse) when only
    // a short name is provided.
    if (!BlueprintName.StartsWith(TEXT("/")))
    {
        TArray<FAssetData> FoundAssets;
        AssetRegistryModule.Get().GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), FoundAssets);
        for (const FAssetData& Asset : FoundAssets)
        {
            if (Asset.AssetName.ToString() == BlueprintName)
            {
                Blueprint = Cast<UBlueprint>(Asset.GetAsset());
                if (Blueprint)
                {
                    UE_LOG(LogUnrealMCP, Display, TEXT("FindBlueprintByName: Found '%s' via Asset Registry at %s"), *BlueprintName, *Asset.GetObjectPathString());
                    return Blueprint;
                }
            }
        }
    }

    UE_LOG(LogUnrealMCP, Error, TEXT("FindBlueprintByName: Failed to find or load blueprint: %s"), *BlueprintName);
    return nullptr;
}

UClass* FMCPCommonUtils::ResolveClass(const FString& Name)
{
    if (Name.IsEmpty())
    {
        return nullptr;
    }

    // 1. Object/asset path → LoadClass. Append "_C" for a bare asset path.
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
        // Fall back to loading the Blueprint asset itself and taking its
        // GeneratedClass (handles unloaded /Game assets robustly).
        if (UBlueprint* BP = FindBlueprintByName(Name))
        {
            if (BP->GeneratedClass)
            {
                return BP->GeneratedClass;
            }
        }
    }

    // 2. Exact short-name match against loaded classes (UClass::GetName has no
    //    U/A/F prefix, so "Pawn"/"Actor" match directly).
    if (UClass* Found = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::ExactClass))
    {
        return Found;
    }

    // 3. Strip / add a U or A prefix for native classes.
    if (Name.Len() > 1 && (Name[0] == TEXT('A') || Name[0] == TEXT('U')))
    {
        if (UClass* Found = FindFirstObject<UClass>(*Name.RightChop(1), EFindFirstObjectOptions::ExactClass))
        {
            return Found;
        }
    }
    else
    {
        for (const TCHAR* Prefix : { TEXT("A"), TEXT("U") })
        {
            const FString Prefixed = FString::Printf(TEXT("%s%s"), Prefix, *Name);
            if (UClass* Found = FindFirstObject<UClass>(*Prefixed, EFindFirstObjectOptions::ExactClass))
            {
                return Found;
            }
        }
    }

    // 4. Bare Blueprint asset name ("BP_Foo") → its GeneratedClass. Loads the
    //    asset through the Asset Registry when it isn't in memory yet.
    if (UBlueprint* BP = FindBlueprintByName(Name))
    {
        if (BP->GeneratedClass)
        {
            return BP->GeneratedClass;
        }
    }

    return nullptr;
}

UEdGraph* FMCPCommonUtils::FindOrCreateEventGraph(UBlueprint* Blueprint)
{
    if (!Blueprint)
    {
        return nullptr;
    }
    
    // Try to find the event graph
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (Graph->GetName().Contains(TEXT("EventGraph")))
        {
            return Graph;
        }
    }
    
    // Only auto-create event graphs for normal Actor blueprints.
    // AnimBlueprints, WidgetBlueprints, FunctionLibraries, and Interfaces use
    // different graph types — creating a K2 event graph on them would be nonsensical.
    if (Blueprint->BlueprintType != BPTYPE_Normal)
    {
        UE_LOG(LogUnrealMCP, Warning, TEXT("FindOrCreateEventGraph: Blueprint '%s' is type %d (not Normal) — cannot create event graph"),
            *Blueprint->GetName(), (int32)Blueprint->BlueprintType);
        return nullptr;
    }

    // Create a new event graph if none exists
    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(TEXT("EventGraph")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddUbergraphPage(Blueprint, NewGraph);
    UE_LOG(LogUnrealMCP, Display, TEXT("FindOrCreateEventGraph: Created new EventGraph for '%s'"), *Blueprint->GetName());
    return NewGraph;
}

UEdGraph* FMCPCommonUtils::FindGraphByName(UBlueprint* Blueprint, const FString& GraphName,
    const FString& TransFromState, const FString& TransToState)
{
    if (!Blueprint) return nullptr;

    const bool bFilterByStates = !TransFromState.IsEmpty() && !TransToState.IsEmpty();

    // If no graph name and no state filter, nothing to search for.
    if (GraphName.IsEmpty() && !bFilterByStates) return nullptr;

    // Search UbergraphPages (EventGraph, etc.)
    for (UEdGraph* G : Blueprint->UbergraphPages)
    {
        if (G && G->GetName() == GraphName) return G;
    }

    // Search FunctionGraphs (includes AnimGraph for AnimBPs)
    for (UEdGraph* G : Blueprint->FunctionGraphs)
    {
        if (G && G->GetName() == GraphName) return G;
    }

    // Search DelegateSignatureGraphs
    for (UEdGraph* G : Blueprint->DelegateSignatureGraphs)
    {
        if (G && G->GetName() == GraphName) return G;
    }

    // Search state machine sub-graphs (state machine graphs, per-state inner graphs, transition rules)
    auto SearchStateMachines = [&GraphName, bFilterByStates, &TransFromState, &TransToState](const TArray<UEdGraph*>& GraphList) -> UEdGraph*
    {
        for (UEdGraph* G : GraphList)
        {
            if (!G) continue;
            for (UEdGraphNode* Node : G->Nodes)
            {
                UAnimGraphNode_StateMachine* SM = Cast<UAnimGraphNode_StateMachine>(Node);
                if (!SM || !SM->EditorStateMachineGraph) continue;

                if (SM->EditorStateMachineGraph->GetName() == GraphName)
                    return SM->EditorStateMachineGraph;

                for (UEdGraphNode* SMNode : SM->EditorStateMachineGraph->Nodes)
                {
                    if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(SMNode))
                    {
                        if (StateNode->BoundGraph && StateNode->BoundGraph->GetName() == GraphName)
                            return StateNode->BoundGraph;
                    }
                    else if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(SMNode))
                    {
                        if (!TransNode->BoundGraph) continue;

                        if (bFilterByStates)
                        {
                            // Disambiguate by from/to state names
                            const FString From = TransNode->GetPreviousState()
                                ? TransNode->GetPreviousState()->GetStateName() : TEXT("");
                            const FString To = TransNode->GetNextState()
                                ? TransNode->GetNextState()->GetStateName() : TEXT("");
                            if (From == TransFromState && To == TransToState)
                                return TransNode->BoundGraph;
                        }
                        else if (TransNode->BoundGraph->GetName() == GraphName)
                        {
                            return TransNode->BoundGraph;
                        }
                    }
                }
            }
        }
        return nullptr;
    };

    if (UEdGraph* Found = SearchStateMachines(Blueprint->UbergraphPages)) return Found;
    if (UEdGraph* Found = SearchStateMachines(Blueprint->FunctionGraphs)) return Found;

    return nullptr;
}

// Blueprint node utilities
UK2Node_Event* FMCPCommonUtils::CreateEventNode(UEdGraph* Graph, const FString& EventName, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }
    
    UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
    if (!Blueprint)
    {
        return nullptr;
    }
    
    // Check for existing event node with this exact name
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
        if (EventNode && EventNode->EventReference.GetMemberName() == FName(*EventName))
        {
            UE_LOG(LogUnrealMCP, Display, TEXT("Using existing event node with name %s (ID: %s)"), 
                *EventName, *EventNode->NodeGuid.ToString());
            return EventNode;
        }
    }

    // No existing node found, create a new one
    UK2Node_Event* EventNode = nullptr;
    
    // Find the function to create the event
    UClass* BlueprintClass = Blueprint->GeneratedClass;
    if (!BlueprintClass)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("CreateEventNode: Blueprint '%s' has no GeneratedClass (uncompiled?) — cannot create event node"), *Blueprint->GetName());
        return nullptr;
    }
    UFunction* EventFunction = BlueprintClass->FindFunctionByName(FName(*EventName));
    
    if (EventFunction)
    {
        EventNode = NewObject<UK2Node_Event>(Graph);
        EventNode->EventReference.SetExternalMember(FName(*EventName), BlueprintClass);
        EventNode->NodePosX = Position.X;
        EventNode->NodePosY = Position.Y;
        Graph->AddNode(EventNode, true);
        EventNode->PostPlacedNewNode();
        EventNode->AllocateDefaultPins();
        UE_LOG(LogUnrealMCP, Display, TEXT("Created new event node with name %s (ID: %s)"), 
            *EventName, *EventNode->NodeGuid.ToString());
    }
    else
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Failed to find function for event name: %s"), *EventName);
    }
    
    return EventNode;
}

UK2Node_CallFunction* FMCPCommonUtils::CreateFunctionCallNode(UEdGraph* Graph, UFunction* Function, const FVector2D& Position)
{
    if (!Graph || !Function)
    {
        return nullptr;
    }
    
    UK2Node_CallFunction* FunctionNode = NewObject<UK2Node_CallFunction>(Graph);
    FunctionNode->SetFromFunction(Function);
    FunctionNode->NodePosX = Position.X;
    FunctionNode->NodePosY = Position.Y;
    Graph->AddNode(FunctionNode, true);
    FunctionNode->CreateNewGuid();
    FunctionNode->PostPlacedNewNode();
    FunctionNode->AllocateDefaultPins();
    
    return FunctionNode;
}

UK2Node_VariableGet* FMCPCommonUtils::CreateVariableGetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position)
{
    if (!Graph || !Blueprint)
    {
        return nullptr;
    }
    
    UK2Node_VariableGet* VariableGetNode = NewObject<UK2Node_VariableGet>(Graph);
    
    FName VarName(*VariableName);
    FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName);
    
    if (Property)
    {
        VariableGetNode->VariableReference.SetFromField<FProperty>(Property, false);
        VariableGetNode->NodePosX = Position.X;
        VariableGetNode->NodePosY = Position.Y;
        Graph->AddNode(VariableGetNode, true);
        VariableGetNode->PostPlacedNewNode();
        VariableGetNode->AllocateDefaultPins();
        
        return VariableGetNode;
    }
    
    return nullptr;
}

UK2Node_VariableSet* FMCPCommonUtils::CreateVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position)
{
    if (!Graph || !Blueprint)
    {
        return nullptr;
    }
    
    UK2Node_VariableSet* VariableSetNode = NewObject<UK2Node_VariableSet>(Graph);
    
    FName VarName(*VariableName);
    FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName);
    
    if (Property)
    {
        VariableSetNode->VariableReference.SetFromField<FProperty>(Property, false);
        VariableSetNode->NodePosX = Position.X;
        VariableSetNode->NodePosY = Position.Y;
        Graph->AddNode(VariableSetNode, true);
        VariableSetNode->PostPlacedNewNode();
        VariableSetNode->AllocateDefaultPins();
        
        return VariableSetNode;
    }
    
    return nullptr;
}

UK2Node_InputAction* FMCPCommonUtils::CreateInputActionNode(UEdGraph* Graph, const FString& ActionName, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }
    
    UK2Node_InputAction* InputActionNode = NewObject<UK2Node_InputAction>(Graph);
    InputActionNode->InputActionName = FName(*ActionName);
    InputActionNode->NodePosX = Position.X;
    InputActionNode->NodePosY = Position.Y;
    Graph->AddNode(InputActionNode, true);
    InputActionNode->CreateNewGuid();
    InputActionNode->PostPlacedNewNode();
    InputActionNode->AllocateDefaultPins();
    
    return InputActionNode;
}

UK2Node_Self* FMCPCommonUtils::CreateSelfReferenceNode(UEdGraph* Graph, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }
    
    UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Graph);
    SelfNode->NodePosX = Position.X;
    SelfNode->NodePosY = Position.Y;
    Graph->AddNode(SelfNode, true);
    SelfNode->CreateNewGuid();
    SelfNode->PostPlacedNewNode();
    SelfNode->AllocateDefaultPins();
    
    return SelfNode;
}

bool FMCPCommonUtils::ConnectGraphNodes(UEdGraph* Graph, UEdGraphNode* SourceNode, const FString& SourcePinName, 
                                           UEdGraphNode* TargetNode, const FString& TargetPinName)
{
    if (!Graph || !SourceNode || !TargetNode)
    {
        return false;
    }
    
    UEdGraphPin* SourcePin = FindPin(SourceNode, SourcePinName, EGPD_Output);
    UEdGraphPin* TargetPin = FindPin(TargetNode, TargetPinName, EGPD_Input);
    
    if (SourcePin && TargetPin)
    {
        SourcePin->MakeLinkTo(TargetPin);
        return true;
    }
    
    return false;
}

UEdGraphPin* FMCPCommonUtils::FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
{
    if (!Node)
    {
        return nullptr;
    }
    
    // Log all pins for debugging
    UE_LOG(LogUnrealMCP, Display, TEXT("FindPin: Looking for pin '%s' (Direction: %d) in node '%s'"), 
           *PinName, (int32)Direction, *Node->GetName());
    
    for (UEdGraphPin* Pin : Node->Pins)
    {
        UE_LOG(LogUnrealMCP, Display, TEXT("  - Available pin: '%s', Direction: %d, Category: %s"), 
               *Pin->PinName.ToString(), (int32)Pin->Direction, *Pin->PinType.PinCategory.ToString());
    }
    
    // First try exact match
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->PinName.ToString() == PinName && (Direction == EGPD_MAX || Pin->Direction == Direction))
        {
            UE_LOG(LogUnrealMCP, Display, TEXT("  - Found exact matching pin: '%s'"), *Pin->PinName.ToString());
            return Pin;
        }
    }
    
    // If no exact match and we're looking for a component reference, try case-insensitive match
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase) && 
            (Direction == EGPD_MAX || Pin->Direction == Direction))
        {
            UE_LOG(LogUnrealMCP, Display, TEXT("  - Found case-insensitive matching pin: '%s'"), *Pin->PinName.ToString());
            return Pin;
        }
    }
    
    // If we're looking for a component output and didn't find it by name, try to find the first data output pin
    if (Direction == EGPD_Output && Cast<UK2Node_VariableGet>(Node) != nullptr)
    {
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                UE_LOG(LogUnrealMCP, Display, TEXT("  - Found fallback data output pin: '%s'"), *Pin->PinName.ToString());
                return Pin;
            }
        }
    }
    
    UE_LOG(LogUnrealMCP, Warning, TEXT("  - No matching pin found for '%s'"), *PinName);
    return nullptr;
}

// Actor utilities
TSharedPtr<FJsonValue> FMCPCommonUtils::ActorToJson(AActor* Actor)
{
    if (!Actor)
    {
        return MakeShared<FJsonValueNull>();
    }
    
    TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
    ActorObject->SetStringField(TEXT("name"), Actor->GetName());
    ActorObject->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
    
    FVector Location = Actor->GetActorLocation();
    TArray<TSharedPtr<FJsonValue>> LocationArray;
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
    ActorObject->SetArrayField(TEXT("location"), LocationArray);
    
    FRotator Rotation = Actor->GetActorRotation();
    TArray<TSharedPtr<FJsonValue>> RotationArray;
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
    ActorObject->SetArrayField(TEXT("rotation"), RotationArray);
    
    FVector Scale = Actor->GetActorScale3D();
    TArray<TSharedPtr<FJsonValue>> ScaleArray;
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
    ActorObject->SetArrayField(TEXT("scale"), ScaleArray);
    
    return MakeShared<FJsonValueObject>(ActorObject);
}

TSharedPtr<FJsonObject> FMCPCommonUtils::ActorToJsonObject(AActor* Actor, bool bDetailed)
{
    if (!Actor)
    {
        return nullptr;
    }
    
    TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
    ActorObject->SetStringField(TEXT("name"), Actor->GetName());
    ActorObject->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
    
    FVector Location = Actor->GetActorLocation();
    TArray<TSharedPtr<FJsonValue>> LocationArray;
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
    ActorObject->SetArrayField(TEXT("location"), LocationArray);
    
    FRotator Rotation = Actor->GetActorRotation();
    TArray<TSharedPtr<FJsonValue>> RotationArray;
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
    ActorObject->SetArrayField(TEXT("rotation"), RotationArray);
    
    FVector Scale = Actor->GetActorScale3D();
    TArray<TSharedPtr<FJsonValue>> ScaleArray;
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
    ActorObject->SetArrayField(TEXT("scale"), ScaleArray);
    
    return ActorObject;
}

UK2Node_Event* FMCPCommonUtils::FindExistingEventNode(UEdGraph* Graph, const FString& EventName)
{
    if (!Graph)
    {
        return nullptr;
    }

    // Look for existing event nodes
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
        if (EventNode && EventNode->EventReference.GetMemberName() == FName(*EventName))
        {
            UE_LOG(LogUnrealMCP, Display, TEXT("Found existing event node with name: %s"), *EventName);
            return EventNode;
        }
    }

    return nullptr;
}

bool FMCPCommonUtils::SetObjectProperty(UObject* Object, const FString& PropertyName, 
                                     const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage)
{
    if (!Object)
    {
        OutErrorMessage = TEXT("Invalid object");
        return false;
    }

    FProperty* Property = Object->GetClass()->FindPropertyByName(*PropertyName);
    if (!Property)
    {
        OutErrorMessage = FString::Printf(TEXT("Property not found: %s"), *PropertyName);
        return false;
    }

    void* PropertyAddr = Property->ContainerPtrToValuePtr<void>(Object);
    
    // Handle different property types
    if (Property->IsA<FBoolProperty>())
    {
        ((FBoolProperty*)Property)->SetPropertyValue(PropertyAddr, Value->AsBool());
        return true;
    }
    else if (Property->IsA<FIntProperty>())
    {
        int32 IntValue = static_cast<int32>(Value->AsNumber());
        FIntProperty* IntProperty = CastField<FIntProperty>(Property);
        if (IntProperty)
        {
            IntProperty->SetPropertyValue_InContainer(Object, IntValue);
            return true;
        }
    }
    else if (Property->IsA<FFloatProperty>())
    {
        ((FFloatProperty*)Property)->SetPropertyValue(PropertyAddr, Value->AsNumber());
        return true;
    }
    else if (Property->IsA<FStrProperty>())
    {
        ((FStrProperty*)Property)->SetPropertyValue(PropertyAddr, Value->AsString());
        return true;
    }
    else if (FTextProperty* TextProp = CastField<FTextProperty>(Property))
    {
        // GAP-036: FText was previously unsupported, so widget_set_property could not
        // set a TextBlock's Text (or any FText UPROPERTY). Accept a plain JSON string
        // (the common case) — wrap it as a culture-invariant FText. A JSON object form
        // {"string": "..."} is also accepted for callers that pass it structured.
        FString TextStr;
        if (Value->Type == EJson::String)
        {
            TextStr = Value->AsString();
        }
        else if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject>* Obj = nullptr;
            if (Value->TryGetObject(Obj) && Obj && Obj->IsValid())
            {
                (*Obj)->TryGetStringField(TEXT("string"), TextStr);
            }
        }
        else
        {
            TextStr = Value->AsString();
        }
        TextProp->SetPropertyValue(PropertyAddr, FText::FromString(TextStr));
        return true;
    }
    else if (Property->IsA<FByteProperty>())
    {
        FByteProperty* ByteProp = CastField<FByteProperty>(Property);
        UEnum* EnumDef = ByteProp ? ByteProp->GetIntPropertyEnum() : nullptr;
        
        // If this is a TEnumAsByte property (has associated enum)
        if (EnumDef)
        {
            // Handle numeric value
            if (Value->Type == EJson::Number)
            {
                uint8 ByteValue = static_cast<uint8>(Value->AsNumber());
                ByteProp->SetPropertyValue(PropertyAddr, ByteValue);
                
                UE_LOG(LogUnrealMCP, Display, TEXT("Setting enum property %s to numeric value: %d"), 
                      *PropertyName, ByteValue);
                return true;
            }
            // Handle string enum value
            else if (Value->Type == EJson::String)
            {
                FString EnumValueName = Value->AsString();
                
                // Try to convert numeric string to number first
                if (EnumValueName.IsNumeric())
                {
                    uint8 ByteValue = FCString::Atoi(*EnumValueName);
                    ByteProp->SetPropertyValue(PropertyAddr, ByteValue);
                    
                    UE_LOG(LogUnrealMCP, Display, TEXT("Setting enum property %s to numeric string value: %s -> %d"), 
                          *PropertyName, *EnumValueName, ByteValue);
                    return true;
                }
                
                // Handle qualified enum names (e.g., "Player0" or "EAutoReceiveInput::Player0")
                if (EnumValueName.Contains(TEXT("::")))
                {
                    EnumValueName.Split(TEXT("::"), nullptr, &EnumValueName);
                }
                
                int64 EnumValue = EnumDef->GetValueByNameString(EnumValueName);
                if (EnumValue == INDEX_NONE)
                {
                    // Try with full name as fallback
                    EnumValue = EnumDef->GetValueByNameString(Value->AsString());
                }
                
                if (EnumValue != INDEX_NONE)
                {
                    ByteProp->SetPropertyValue(PropertyAddr, static_cast<uint8>(EnumValue));
                    
                    UE_LOG(LogUnrealMCP, Display, TEXT("Setting enum property %s to name value: %s -> %lld"), 
                          *PropertyName, *EnumValueName, EnumValue);
                    return true;
                }
                else
                {
                    // Log all possible enum values for debugging
                    UE_LOG(LogUnrealMCP, Warning, TEXT("Could not find enum value for '%s'. Available options:"), *EnumValueName);
                    for (int32 i = 0; i < EnumDef->NumEnums(); i++)
                    {
                        UE_LOG(LogUnrealMCP, Warning, TEXT("  - %s (value: %d)"), 
                               *EnumDef->GetNameStringByIndex(i), EnumDef->GetValueByIndex(i));
                    }
                    
                    OutErrorMessage = FString::Printf(TEXT("Could not find enum value for '%s'"), *EnumValueName);
                    return false;
                }
            }
        }
        else
        {
            // Regular byte property
            uint8 ByteValue = static_cast<uint8>(Value->AsNumber());
            ByteProp->SetPropertyValue(PropertyAddr, ByteValue);
            return true;
        }
    }
    else if (Property->IsA<FEnumProperty>())
    {
        FEnumProperty* EnumProp = CastField<FEnumProperty>(Property);
        UEnum* EnumDef = EnumProp ? EnumProp->GetEnum() : nullptr;
        FNumericProperty* UnderlyingNumericProp = EnumProp ? EnumProp->GetUnderlyingProperty() : nullptr;
        
        if (EnumDef && UnderlyingNumericProp)
        {
            // Handle numeric value
            if (Value->Type == EJson::Number)
            {
                int64 EnumValue = static_cast<int64>(Value->AsNumber());
                UnderlyingNumericProp->SetIntPropertyValue(PropertyAddr, EnumValue);
                
                UE_LOG(LogUnrealMCP, Display, TEXT("Setting enum property %s to numeric value: %lld"), 
                      *PropertyName, EnumValue);
                return true;
            }
            // Handle string enum value
            else if (Value->Type == EJson::String)
            {
                FString EnumValueName = Value->AsString();
                
                // Try to convert numeric string to number first
                if (EnumValueName.IsNumeric())
                {
                    int64 EnumValue = FCString::Atoi64(*EnumValueName);
                    UnderlyingNumericProp->SetIntPropertyValue(PropertyAddr, EnumValue);
                    
                    UE_LOG(LogUnrealMCP, Display, TEXT("Setting enum property %s to numeric string value: %s -> %lld"), 
                          *PropertyName, *EnumValueName, EnumValue);
                    return true;
                }
                
                // Handle qualified enum names
                if (EnumValueName.Contains(TEXT("::")))
                {
                    EnumValueName.Split(TEXT("::"), nullptr, &EnumValueName);
                }
                
                int64 EnumValue = EnumDef->GetValueByNameString(EnumValueName);
                if (EnumValue == INDEX_NONE)
                {
                    // Try with full name as fallback
                    EnumValue = EnumDef->GetValueByNameString(Value->AsString());
                }
                
                if (EnumValue != INDEX_NONE)
                {
                    UnderlyingNumericProp->SetIntPropertyValue(PropertyAddr, EnumValue);
                    
                    UE_LOG(LogUnrealMCP, Display, TEXT("Setting enum property %s to name value: %s -> %lld"), 
                          *PropertyName, *EnumValueName, EnumValue);
                    return true;
                }
                else
                {
                    // Log all possible enum values for debugging
                    UE_LOG(LogUnrealMCP, Warning, TEXT("Could not find enum value for '%s'. Available options:"), *EnumValueName);
                    for (int32 i = 0; i < EnumDef->NumEnums(); i++)
                    {
                        UE_LOG(LogUnrealMCP, Warning, TEXT("  - %s (value: %d)"), 
                               *EnumDef->GetNameStringByIndex(i), EnumDef->GetValueByIndex(i));
                    }
                    
                    OutErrorMessage = FString::Printf(TEXT("Could not find enum value for '%s'"), *EnumValueName);
                    return false;
                }
            }
        }
    }
    else if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
    {
        // Generic struct support. Accepts either:
        //   - a JSON object, deserialised field-by-field (FSlateBrush, FAnchors,
        //     FMargin, FLinearColor, FVector2D, ...); object-reference members such
        //     as FSlateBrush.ResourceObject resolve from their string asset path, or
        //   - a UE export-text string ("(Left=0,Top=0,...)"), imported verbatim.
        // Only the fields present are written; omitted members keep their defaults.
        // A JSON object value (or a string carrying JSON — some bridge layers
        // stringify nested values) is deserialised field-by-field.
        const TSharedPtr<FJsonObject>* StructObj = nullptr;
        TSharedPtr<FJsonObject> ParsedObj;
        if (Value->TryGetObject(StructObj) && StructObj && StructObj->IsValid())
        {
            ParsedObj = *StructObj;
        }
        else if (Value->Type == EJson::String)
        {
            const FString Text = Value->AsString();
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
            if (FJsonSerializer::Deserialize(Reader, ParsedObj) && ParsedObj.IsValid())
            {
                // parsed as JSON object below
            }
            else
            {
                // Not JSON — treat as a UE export-text struct literal.
                if (StructProp->ImportText_Direct(*Text, PropertyAddr, Object, PPF_None, nullptr) != nullptr)
                {
                    return true;
                }
                OutErrorMessage = FString::Printf(
                    TEXT("Failed to import struct '%s' (%s) from text: %s"),
                    *PropertyName, *StructProp->Struct->GetName(), *Text);
                return false;
            }
        }

        if (ParsedObj.IsValid())
        {
            if (FJsonObjectConverter::JsonObjectToUStruct(
                    ParsedObj.ToSharedRef(), StructProp->Struct, PropertyAddr, 0, 0))
            {
                return true;
            }
            OutErrorMessage = FString::Printf(
                TEXT("Failed to convert JSON object to struct '%s' (%s)"),
                *PropertyName, *StructProp->Struct->GetName());
            return false;
        }

        OutErrorMessage = FString::Printf(
            TEXT("Struct property '%s' (%s) requires a JSON object or UE export-text string"),
            *PropertyName, *StructProp->Struct->GetName());
        return false;
    }
    else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
    {
        // GAP-064: object/class-reference properties — a component's StaticMesh, a
        // material, a mesh, or any UObject*/UClass*/soft-ref. These previously fell
        // through to the "Unsupported property type" bail, so a Blueprint component
        // template's mesh/material could be neither assigned nor cleared over MCP
        // (only actor_set_property, on placed instances, could). Mirror that handler's
        // resolution: FJsonObjectConverter::JsonValueToUProperty loads the asset/class
        // from its path string (ImportText) and validates class compatibility against
        // the property's PropertyClass/MetaClass. Then add an explicit clear path —
        // a JSON null / "" / "None" nulls the reference (ImportText alone won't clear
        // on an empty string). Covers FObjectProperty (incl. FClassProperty),
        // FSoftObjectProperty and FSoftClassProperty (all FObjectPropertyBase).
        const bool bClear =
            Value->Type == EJson::Null ||
            (Value->Type == EJson::String &&
                (Value->AsString().IsEmpty() || Value->AsString().Equals(TEXT("None"), ESearchCase::IgnoreCase)));
        if (bClear)
        {
            ObjProp->SetObjectPropertyValue(PropertyAddr, nullptr);
            return true;
        }

        FText FailReason;
        if (FJsonObjectConverter::JsonValueToUProperty(Value, Property, PropertyAddr, 0, 0, false, &FailReason))
        {
            return true;
        }
        OutErrorMessage = FString::Printf(
            TEXT("Failed to resolve object reference for property '%s' (%s): %s"),
            *PropertyName, *Property->GetCPPType(),
            FailReason.IsEmpty()
                ? TEXT("no asset/class at that path, or its class is incompatible with the property. Pass a full asset path (e.g. /Game/Path/Asset.Asset), a class path (…_C for a Blueprint class), or null/\"None\" to clear.")
                : *FailReason.ToString());
        return false;
    }

    OutErrorMessage = FString::Printf(TEXT("Unsupported property type: %s for property %s"),
                                    *Property->GetClass()->GetName(), *PropertyName);
    return false;
}

bool FMCPCommonUtils::SetObjectPropertyByPath(UObject* Root, const FString& DottedPath,
    const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage)
{
    // Dotted-path setter: walks struct/object segments (e.g.
    // "BodyInstance.bNotifyRigidBodyCollision", "SpringArm.TargetArmLength") and sets
    // the leaf. Single-segment paths and object leaves reuse the rich, type-aware
    // SetObjectProperty (enums-by-name, FText, whole-struct merge); leaves living in
    // raw struct memory go through FJsonObjectConverter. Used by component-property
    // application (bp_add_component) and bp_set_component_property.
    if (!Root)
    {
        OutErrorMessage = TEXT("Invalid object");
        return false;
    }

    TArray<FString> Segments;
    DottedPath.ParseIntoArray(Segments, TEXT("."), /*CullEmpty=*/true);
    if (Segments.Num() == 0)
    {
        OutErrorMessage = TEXT("Empty property path");
        return false;
    }

    if (Segments.Num() == 1)
    {
        return SetObjectProperty(Root, Segments[0], Value, OutErrorMessage);
    }

    void* ContainerPtr = static_cast<void*>(Root);
    UStruct* CurrentStruct = Root->GetClass();
    UObject* OwnerObject = Root;
    bool bContainerIsObject = true;

    for (int32 i = 0; i < Segments.Num() - 1; ++i)
    {
        FProperty* Prop = CurrentStruct->FindPropertyByName(FName(*Segments[i]));
        if (!Prop)
        {
            OutErrorMessage = FString::Printf(TEXT("Path segment '%s' not found on %s"), *Segments[i], *CurrentStruct->GetName());
            return false;
        }

        if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
        {
            ContainerPtr = StructProp->ContainerPtrToValuePtr<void>(ContainerPtr);
            CurrentStruct = StructProp->Struct;
            bContainerIsObject = false;
        }
        else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
        {
            UObject* Sub = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(ContainerPtr));
            if (!Sub)
            {
                OutErrorMessage = FString::Printf(TEXT("Path segment '%s' is a null object reference — cannot descend"), *Segments[i]);
                return false;
            }
            OwnerObject = Sub;
            ContainerPtr = static_cast<void*>(Sub);
            CurrentStruct = Sub->GetClass();
            bContainerIsObject = true;
        }
        else
        {
            OutErrorMessage = FString::Printf(TEXT("Path segment '%s' (%s) is neither a struct nor an object — cannot descend"), *Segments[i], *Prop->GetCPPType());
            return false;
        }
    }

    const FString& LeafName = Segments.Last();
    if (bContainerIsObject)
    {
        return SetObjectProperty(OwnerObject, LeafName, Value, OutErrorMessage);
    }

    FProperty* LeafProp = CurrentStruct->FindPropertyByName(FName(*LeafName));
    if (!LeafProp)
    {
        OutErrorMessage = FString::Printf(TEXT("Leaf property '%s' not found on %s"), *LeafName, *CurrentStruct->GetName());
        return false;
    }
    void* LeafPtr = LeafProp->ContainerPtrToValuePtr<void>(ContainerPtr);
    if (!FJsonObjectConverter::JsonValueToUProperty(Value, LeafProp, LeafPtr, 0, 0))
    {
        OutErrorMessage = FString::Printf(TEXT("Failed to convert value for '%s' (type=%s)"), *LeafName, *LeafProp->GetCPPType());
        return false;
    }
    return true;
}