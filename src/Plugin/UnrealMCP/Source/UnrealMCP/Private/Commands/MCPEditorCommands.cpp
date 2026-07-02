#include "Commands/MCPEditorCommands.h"
#include "Commands/MCPCommonUtils.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/GameModeBase.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "ImageUtils.h"
#include "HighResScreenshot.h"
#include "Engine/GameViewportClient.h"
#include "Misc/FileHelper.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "Components/StaticMeshComponent.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EditorAssetLibrary.h"
#include "Commands/MCPBlueprintCommands.h"
#include "JsonObjectConverter.h"
#include "UObject/UnrealType.h"
#include "UObject/Field.h"
#include "UObject/Package.h"

FMCPEditorCommands::FMCPEditorCommands()
{
}

TSharedPtr<FJsonObject> FMCPEditorCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    // Actor manipulation commands
    if (CommandType == TEXT("actor_get_in_level"))
    {
        return HandleGetActorsInLevel(Params);
    }
    else if (CommandType == TEXT("find_actors_by_name"))
    {
        return HandleFindActorsByName(Params);
    }
    else if (CommandType == TEXT("spawn_actor"))
    {
        return HandleSpawnActor(Params);
    }
    else if (CommandType == TEXT("actor_delete"))
    {
        return HandleDeleteActor(Params);
    }
    else if (CommandType == TEXT("actor_set_transform"))
    {
        return HandleSetActorTransform(Params);
    }
    else if (CommandType == TEXT("actor_set_property"))
    {
        return HandleSetActorProperty(Params);
    }
    // Blueprint actor spawning
    else if (CommandType == TEXT("spawn_blueprint_actor"))
    {
        return HandleSpawnBlueprintActor(Params);
    }
    // Data reading
    else if (CommandType == TEXT("asset_datatable_read"))
    {
        return HandleReadDataTable(Params);
    }
    // Level / world settings
    else if (CommandType == TEXT("level_set_gamemode_override"))
    {
        return HandleSetWorldGameModeOverride(Params);
    }

    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown editor command: %s"), *CommandType),
        EMCPErrorCode::InvalidArgument,
        TEXT("`command` must be one of: get_actors_in_level, find_actors_by_name, spawn_actor, delete_actor, set_actor_transform, actor_set_property, spawn_blueprint_actor, read_data_table, set_world_gamemode_override."));
}

TSharedPtr<FJsonObject> FMCPEditorCommands::HandleGetActorsInLevel(const TSharedPtr<FJsonObject>& Params)
{
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    TArray<TSharedPtr<FJsonValue>> ActorArray;
    for (AActor* Actor : AllActors)
    {
        if (Actor)
        {
            ActorArray.Add(FMCPCommonUtils::ActorToJson(Actor));
        }
    }
    
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("actors"), ActorArray);
    
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPEditorCommands::HandleFindActorsByName(const TSharedPtr<FJsonObject>& Params)
{
    FString Pattern;
    if (!Params->TryGetStringField(TEXT("pattern"), Pattern))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'pattern' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`pattern` is required (string, substring matched against actor `GetName()`). Match is case-sensitive `Contains` — pass any unique substring of the target actor's name."));
    }
    
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    TArray<TSharedPtr<FJsonValue>> MatchingActors;
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName().Contains(Pattern))
        {
            MatchingActors.Add(FMCPCommonUtils::ActorToJson(Actor));
        }
    }
    
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("actors"), MatchingActors);
    
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPEditorCommands::HandleSpawnActor(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString ActorType;
    if (!Params->TryGetStringField(TEXT("type"), ActorType))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'type' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`type` is required (string). Supported types: StaticMeshActor, PointLight, SpotLight, DirectionalLight, CameraActor. To spawn a Blueprint actor, use `spawn_blueprint_actor` instead with the Blueprint's asset path."));
    }

    // Get actor name (required parameter)
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`name` is required (string, the actor's UObject name as seen by `GetName()`, NOT the actor label). Use `get_actors_in_level` or `find_actors_by_name` to discover actor names in the current world."));
    }

    // Get optional transform parameters
    FVector Location(0.0f, 0.0f, 0.0f);
    FRotator Rotation(0.0f, 0.0f, 0.0f);
    FVector Scale(1.0f, 1.0f, 1.0f);

    if (Params->HasField(TEXT("location")))
    {
        Location = FMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = FMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
    }
    if (Params->HasField(TEXT("scale")))
    {
        Scale = FMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale"));
    }

    // Create the actor based on type
    AActor* NewActor = nullptr;
    if (!GEditor)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Editor not available"),
            EMCPErrorCode::Internal,
            TEXT("GEditor is null — the editor is not yet (or no longer) initialized. This only happens at editor startup or shutdown. Retry once the editor is fully loaded."));
    }
    UWorld* World = GEditor->GetEditorWorldContext().World();

    if (!World)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Failed to get editor world"),
            EMCPErrorCode::Internal,
            TEXT("GEditor->GetEditorWorldContext().World() returned null. The editor's world context is not yet initialized — typically only happens at editor startup or shutdown. Retry once the editor is fully loaded."));
    }

    // Check if an actor with this name already exists
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Actor with name '%s' already exists"), *ActorName),
                EMCPErrorCode::NameCollision,
                TEXT("An actor with the requested `name` already exists in the editor world. Pick a different name, or call `delete_actor` on the existing actor first if you intend to replace it. Use `find_actors_by_name` to verify."));
        }
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = *ActorName;
    // The default NameMode is Required_Fatal: if the requested name is already in
    // use by ANY UObject in the level (e.g. a just-deleted actor still pending GC,
    // which the GetAllActorsOfClass collision check above does NOT see), SpawnActor
    // calls UE_LOG(Fatal) and hard-crashes the editor. Demote to a graceful null
    // return — the NewActor null check below already turns that into a structured error.
    SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
    // The default NameMode is Required_Fatal: if the requested name is already in
    // use by ANY UObject in the level (e.g. a just-deleted actor still pending GC,
    // which the GetAllActorsOfClass collision check above does NOT see), SpawnActor
    // calls UE_LOG(Fatal) and hard-crashes the editor. Demote to a graceful null
    // return — the NewActor null check below already turns that into a structured error.
    SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;

    if (ActorType == TEXT("StaticMeshActor"))
    {
        AStaticMeshActor* NewMeshActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Location, Rotation, SpawnParams);
        if (NewMeshActor)
        {
            // Check for an optional static_mesh parameter to assign a mesh
            FString MeshPath;
            if (Params->TryGetStringField(TEXT("static_mesh"), MeshPath))
            {
                UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(MeshPath));
                if (Mesh)
                {
                    NewMeshActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
                }
                else
                {
                    UE_LOG(LogUnrealMCP, Warning, TEXT("Could not find static mesh at path: %s"), *MeshPath);
                }
            }
        }
        NewActor = NewMeshActor;
    }
    else if (ActorType == TEXT("PointLight"))
    {
        NewActor = World->SpawnActor<APointLight>(APointLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType == TEXT("SpotLight"))
    {
        NewActor = World->SpawnActor<ASpotLight>(ASpotLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType == TEXT("DirectionalLight"))
    {
        NewActor = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType == TEXT("CameraActor"))
    {
        NewActor = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), Location, Rotation, SpawnParams);
    }
    else
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown actor type: %s"), *ActorType),
            EMCPErrorCode::InvalidArgument,
            TEXT("`type` must be one of the supported native actor classes: StaticMeshActor, PointLight, SpotLight, DirectionalLight, CameraActor (match is case-sensitive). To spawn a Blueprint actor, use `spawn_blueprint_actor` with the BP asset path."));
    }

    if (NewActor)
    {
        // Set scale (since SpawnActor only takes location and rotation)
        FTransform Transform = NewActor->GetTransform();
        Transform.SetScale3D(Scale);
        NewActor->SetActorTransform(Transform);

        // Return the created actor's details
        return FMCPCommonUtils::ActorToJsonObject(NewActor, true);
    }

    return FMCPCommonUtils::CreateErrorResponse(
        TEXT("Failed to create actor"),
        EMCPErrorCode::Internal,
        TEXT("World->SpawnActor returned nullptr despite a validated type. Common causes: (1) the spawn location is inside collision and bNoCollisionFail is off; (2) the actor class is abstract or has a runtime-rejected CDO; (3) the world is in PIE-shutdown. Try a different `location` (e.g. `{x:0,y:0,z:200}` clear of geometry) and retry."));
}

TSharedPtr<FJsonObject> FMCPEditorCommands::HandleDeleteActor(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`name` is required (string, the actor's UObject name as seen by `GetName()`, NOT the actor label). Use `get_actors_in_level` or `find_actors_by_name` to discover actor names in the current world."));
    }

    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            // Store actor info before deletion for the response (and for the
            // dry-run diff — the actor still exists at this point on either path).
            TSharedPtr<FJsonObject> ActorInfo = FMCPCommonUtils::ActorToJsonObject(Actor);

            // dry_run: validation already ran (actor was found). Emit the
            // diff and skip Destroy(). Diff shape per todo/13: {actors_removed:
            // [<actor brief>]}. We pass the same ActorToJsonObject blob the
            // commit response uses so an agent that compares the two paths
            // gets identical content.
            if (FMCPCommonUtils::ParseDryRun(Params))
            {
                TArray<TSharedPtr<FJsonValue>> RemovedArr;
                RemovedArr.Add(MakeShared<FJsonValueObject>(ActorInfo));
                TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
                Diff->SetArrayField(TEXT("actors_removed"), RemovedArr);
                return FMCPCommonUtils::CreateDryRunResponse(Diff);
            }

            // Delete the actor
            Actor->Destroy();

            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetObjectField(TEXT("deleted_actor"), ActorInfo);
            return ResultObj;
        }
    }

    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Actor not found: %s"), *ActorName),
        EMCPErrorCode::ActorNotFound,
        TEXT("No actor in the editor world matches `name` via GetName(). Names are case-sensitive and match the UObject name, not the actor's display label. Use `get_actors_in_level` or `find_actors_by_name` to discover."));
}

TSharedPtr<FJsonObject> FMCPEditorCommands::HandleSetActorTransform(const TSharedPtr<FJsonObject>& Params)
{
    // Get actor name
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`name` is required (string, the actor's UObject name as seen by `GetName()`, NOT the actor label). Use `get_actors_in_level` or `find_actors_by_name` to discover actor names in the current world."));
    }

    // Find the actor
    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
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
            TEXT("No actor in the editor world matches `name` via GetName(). Names are case-sensitive and match the UObject name, not the actor's display label. Use `get_actors_in_level` or `find_actors_by_name` to discover."));
    }

    // Capture the pre-mutation transform — needed by the dry-run diff so the
    // agent sees both before and after. NewTransform is then derived from it
    // and overlaid with whichever fields the caller supplied.
    const FTransform BeforeTransform = TargetActor->GetTransform();
    FTransform NewTransform = BeforeTransform;

    if (Params->HasField(TEXT("location")))
    {
        NewTransform.SetLocation(FMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        NewTransform.SetRotation(FQuat(FMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"))));
    }
    if (Params->HasField(TEXT("scale")))
    {
        NewTransform.SetScale3D(FMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale")));
    }

    // dry_run: every preflight ran (actor lookup, transform parsing). Diff
    // shape per todo/13: {transforms_changed: [{name, before, after}]}.
    // before/after each carry {location, rotation, scale} so the agent can
    // see the exact delta a commit would produce.
    if (FMCPCommonUtils::ParseDryRun(Params))
    {
        auto WriteTransformBlock = [](TSharedPtr<FJsonObject>& Out, const FTransform& Xf)
        {
            const FVector L = Xf.GetLocation();
            const FRotator R = Xf.GetRotation().Rotator();
            const FVector S = Xf.GetScale3D();

            TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
            Loc->SetNumberField(TEXT("x"), L.X);
            Loc->SetNumberField(TEXT("y"), L.Y);
            Loc->SetNumberField(TEXT("z"), L.Z);
            Out->SetObjectField(TEXT("location"), Loc);

            TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
            Rot->SetNumberField(TEXT("pitch"), R.Pitch);
            Rot->SetNumberField(TEXT("yaw"), R.Yaw);
            Rot->SetNumberField(TEXT("roll"), R.Roll);
            Out->SetObjectField(TEXT("rotation"), Rot);

            TSharedPtr<FJsonObject> Scl = MakeShared<FJsonObject>();
            Scl->SetNumberField(TEXT("x"), S.X);
            Scl->SetNumberField(TEXT("y"), S.Y);
            Scl->SetNumberField(TEXT("z"), S.Z);
            Out->SetObjectField(TEXT("scale"), Scl);
        };

        TSharedPtr<FJsonObject> Before = MakeShared<FJsonObject>();
        WriteTransformBlock(Before, BeforeTransform);
        TSharedPtr<FJsonObject> After = MakeShared<FJsonObject>();
        WriteTransformBlock(After, NewTransform);

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), TargetActor->GetName());
        Entry->SetObjectField(TEXT("before"), Before);
        Entry->SetObjectField(TEXT("after"), After);

        TArray<TSharedPtr<FJsonValue>> ChangedArr;
        ChangedArr.Add(MakeShared<FJsonValueObject>(Entry));

        TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
        Diff->SetArrayField(TEXT("transforms_changed"), ChangedArr);
        return FMCPCommonUtils::CreateDryRunResponse(Diff);
    }

    // Set the new transform
    TargetActor->SetActorTransform(NewTransform);

    // Return updated actor info
    return FMCPCommonUtils::ActorToJsonObject(TargetActor, true);
}

// ---------------------------------------------------------------------------
// SetActorProperty  (actor_set_property)
// ---------------------------------------------------------------------------
// Generic reflective writer for any edit-exposed UPROPERTY on a *placed* level
// actor instance — fills the gap between set_actor_transform (transform only)
// and set_data_asset_property (assets, not actors).
//
// `property` is a dotted path. Each segment descends one level:
//   * FStructProperty   → into the struct's fields   (Settings.ColorSaturation)
//   * FObjectProperty…  → into the referenced object  (DirectionalLightComponent.Intensity)
// The leaf is written via FJsonObjectConverter::JsonValueToUProperty — the same
// converter set_data_asset_property uses — so FVector4 / FPostProcessSettings /
// enums / soft-refs / arrays all convert from JSON transparently. A *whole
// struct* write merges (the engine's JsonAttributesToUStruct only writes the
// fields present in the JSON), so an entire post-process grade plus its
// bOverride_* edit-condition flags can land in one call.
//
// Primary motivation: live color-grade iteration on an APostProcessVolume —
// set Settings.bOverride_ColorSaturation=true + Settings.ColorSaturation, see
// it in the viewport, then bake when happy. `save` defaults false so each tweak
// stays out of the .umap until you deliberately commit it.
//
// Persist contract mirrors HandleSetWorldGameModeOverride: Modify +
// PreEditChange + PostEditChangeProperty on the *owning* object (the actor, or
// the component a path descended into) + MarkPackageDirty on the actor.
// PostEditChangeProperty is what refreshes the owner's render state (the PPV /
// light component re-reads its settings) — a bare assignment that skipped it
// would update memory but not the viewport.

// GAP-059: FJsonObjectConverter::JsonValueToUProperty special-cases several core math structs
// (FVector/FRotator/FVector2D/FLinearColor/…) through a custom serializer that REJECTS the plain
// {"X":..,"Y":..,"Z":..} object form — so a whole-struct write failed even though the documented
// shape was correct (and even though the reflected X/Y/Z sub-properties ARE settable, which is why
// the dotted ".X" path worked). This wrapper tries the stock converter first, then falls back for a
// struct leaf: map a JSON object field-by-field via the struct's reflected sub-properties, or
// ImportText a struct STRING ("(X=..,Y=..,Z=..)"). Used by both the dry-run preview and the commit
// so they stay in lockstep.
static bool MCPConvertJsonToProp(const TSharedPtr<FJsonValue>& JV, FProperty* Prop, void* Ptr)
{
    if (!JV.IsValid() || !Prop || !Ptr) { return false; }
    if (FJsonObjectConverter::JsonValueToUProperty(JV, Prop, Ptr, 0, 0)) { return true; }

    if (FStructProperty* SP = CastField<FStructProperty>(Prop))
    {
        if (JV->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Obj = JV->AsObject();
            if (!Obj.IsValid()) { return false; }
            bool bAnySet = false;
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
            {
                FProperty* Sub = SP->Struct->FindPropertyByName(FName(*Pair.Key));
                if (Sub && FJsonObjectConverter::JsonValueToUProperty(Pair.Value, Sub, Sub->ContainerPtrToValuePtr<void>(Ptr), 0, 0))
                {
                    bAnySet = true; // whole-struct write merges: only the keys passed are changed
                }
            }
            return bAnySet;
        }
        if (JV->Type == EJson::String)
        {
            const FString S = JV->AsString();
            return Prop->ImportText_Direct(*S, Ptr, nullptr, PPF_None) != nullptr;
        }
    }
    return false;
}

TSharedPtr<FJsonObject> FMCPEditorCommands::HandleSetActorProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`name` is required (string, the actor's UObject name as seen by `GetName()`, NOT the actor label). Use `get_actors_in_level` or `find_actors_by_name` to discover actor names."));
    }

    FString PropertyPath;
    if (!Params->TryGetStringField(TEXT("property"), PropertyPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'property' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`property` is required (string). It is a dotted path resolved by reflection, e.g. `bUnbound`, `Settings`, `Settings.ColorSaturation`, or `DirectionalLightComponent.Intensity`. Use `class_inspect` (include=['properties']) on the actor's class to discover top-level property names."));
    }

    TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(TEXT("value"));
    if (!JsonValue.IsValid())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'value' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`value` is required — the JSON value to write, passed through FJsonObjectConverter::JsonValueToUProperty against the resolved leaf property. Bool→bool, numeric→number, enum→string of the element name, struct→object whose keys match the UPROPERTY field names (e.g. FVector4 → {\"X\":1,\"Y\":1,\"Z\":1,\"W\":0.85}). A whole-struct write merges: only the keys you pass are changed."));
    }

    // Find the actor (same lookup the sibling actor handlers use).
    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
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
            TEXT("No actor in the editor world matches `name` via GetName(). Names are case-sensitive and match the UObject name, not the actor's display label. Use `get_actors_in_level` or `find_actors_by_name` to discover."));
    }

    // ── Walk the dotted property path ───────────────────────────────────────
    TArray<FString> Segments;
    PropertyPath.ParseIntoArray(Segments, TEXT("."), /*CullEmpty=*/true);
    if (Segments.Num() == 0)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Empty 'property' path"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`property` resolved to zero path segments after splitting on '.'. Pass a non-empty property name, e.g. `bUnbound` or `Settings.ColorSaturation`."));
    }

    UObject* OwnerObject = TargetActor;             // whose Pre/PostEditChange we fire
    void* ContainerPtr = static_cast<void*>(TargetActor);
    UStruct* CurrentStruct = TargetActor->GetClass();
    FProperty* TargetProp = nullptr;
    void* ValuePtr = nullptr;

    for (int32 i = 0; i < Segments.Num(); ++i)
    {
        const bool bIsLast = (i == Segments.Num() - 1);
        FProperty* Prop = CurrentStruct->FindPropertyByName(FName(*Segments[i]));
        if (!Prop)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Property '%s' not found on %s"), *Segments[i], *CurrentStruct->GetName()),
                EMCPErrorCode::InvalidArgument,
                TEXT("A path segment did not resolve to a property on the current class/struct. Names are case-sensitive and match the C++ UPROPERTY identifier. Use `class_inspect` (include=['properties']) to enumerate the actor's properties; nested struct field names match the engine struct (e.g. FPostProcessSettings: `ColorSaturation`, `bOverride_ColorSaturation`)."));
        }

        // Edit-exposure gate applies to the TOP-LEVEL property only — that is the
        // contract surface. Inside a struct or a component you're already
        // authorized by having reached it through an exposed parent. Mirrors the
        // CPF mask FMCPDataAssetCommands::FindEditableProperty uses.
        if (i == 0)
        {
            const uint64 RequiredAny = CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintAssignable;
            if ((Prop->PropertyFlags & RequiredAny) == 0)
            {
                return FMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Top-level property '%s' on %s is not edit-exposed"), *Segments[i], *CurrentStruct->GetName()),
                    EMCPErrorCode::InvalidArgument,
                    TEXT("The top-level property lacks the EditAnywhere / EditDefaultsOnly / BlueprintReadWrite / BlueprintVisible specifier required for external mutation. This is intentional — only the declared edit surface is writable. Use `class_inspect` to see which properties are editable."));
            }
        }

        if (bIsLast)
        {
            TargetProp = Prop;
            ValuePtr = Prop->ContainerPtrToValuePtr<void>(ContainerPtr);
            break;
        }

        // Descend one level for a non-terminal segment.
        if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
        {
            ContainerPtr = StructProp->ContainerPtrToValuePtr<void>(ContainerPtr);
            CurrentStruct = StructProp->Struct;
        }
        else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
        {
            UObject* SubObject = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(ContainerPtr));
            if (!SubObject)
            {
                return FMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Path segment '%s' is a null object reference — cannot descend"), *Segments[i]),
                    EMCPErrorCode::InvalidArgument,
                    TEXT("An object/component path segment is currently null on this actor, so there is nothing to descend into. Verify the component exists (e.g. via `actor_inspect`, which lists components) and that the reference is set."));
            }
            OwnerObject = SubObject;
            ContainerPtr = static_cast<void*>(SubObject);
            CurrentStruct = SubObject->GetClass();
        }
        else
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Path segment '%s' (type %s) is neither a struct nor an object — cannot descend further"), *Segments[i], *Prop->GetCPPType()),
                EMCPErrorCode::InvalidArgument,
                TEXT("Only FStructProperty and object/component properties can be descended through with a dotted path. The named segment is a leaf value — set it directly (drop the trailing path), or target a struct/object segment to go deeper."));
        }
    }

    // Capture before-text of the resolved leaf for the response / dry-run diff.
    FString BeforeText;
    TargetProp->ExportTextItem_Direct(BeforeText, ValuePtr, nullptr, nullptr, PPF_None);

    // dry_run: all validation has run (actor found, path resolved, value present).
    // Emit the diff without mutating. Shape: {properties_changed: [{name,
    // property, before, requested_value}]} — mirrors the doc-13 diff convention.
    if (FMCPCommonUtils::ParseDryRun(Params))
    {
        // GAP-010: run the SAME conversion the real write performs, into a throwaway
        // buffer, so dry-run can no longer green-light a value the commit rejects (the
        // old path just echoed the raw JSON — e.g. an FVector as [600,600,1] "passed"
        // dry-run but failed the real write, which needs "(X=..,Y=..,Z=..)"-style input).
        void* TempBuffer = FMemory::Malloc(TargetProp->GetSize(), TargetProp->GetMinAlignment());
        TargetProp->InitializeValue(TempBuffer);
        const bool bWouldConvert = MCPConvertJsonToProp(JsonValue, TargetProp, TempBuffer);
        FString PreviewText;
        if (bWouldConvert)
        {
            TargetProp->ExportTextItem_Direct(PreviewText, TempBuffer, nullptr, nullptr, PPF_None);
        }
        TargetProp->DestroyValue(TempBuffer);
        FMemory::Free(TempBuffer);

        if (!bWouldConvert)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Failed to convert 'value' for property '%s' (type=%s)"), *PropertyPath, *TargetProp->GetCPPType()),
                EMCPErrorCode::InvalidArgument,
                TEXT("`value` did not convert through FJsonObjectConverter::JsonValueToUProperty against the leaf property's type (listed above). Match the JSON shape to the CPP type: structs are objects keyed by UPROPERTY field name (e.g. FVector → {\"X\":600,\"Y\":600,\"Z\":1}), enums are strings of the element name, object refs are asset-path strings, arrays are JSON arrays. This is the same check the real write runs — fix the value and re-issue."));
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), TargetActor->GetName());
        Entry->SetStringField(TEXT("property"), PropertyPath);
        Entry->SetStringField(TEXT("property_type"), TargetProp->GetCPPType());
        Entry->SetStringField(TEXT("before"), BeforeText);
        Entry->SetField(TEXT("requested_value"), JsonValue);
        Entry->SetStringField(TEXT("after"), PreviewText);

        TArray<TSharedPtr<FJsonValue>> ChangedArr;
        ChangedArr.Add(MakeShared<FJsonValueObject>(Entry));
        TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
        Diff->SetArrayField(TEXT("properties_changed"), ChangedArr);
        return FMCPCommonUtils::CreateDryRunResponse(Diff);
    }

    // ── Commit ──────────────────────────────────────────────────────────────
    TargetActor->Modify();
    OwnerObject->PreEditChange(TargetProp);
    const bool bConverted = MCPConvertJsonToProp(JsonValue, TargetProp, ValuePtr);

    // CRASH FIX: capture EVERYTHING we need for the response/log NOW, while OwnerObject and
    // ValuePtr are still valid. The very next call — PostEditChangeProperty — can trigger
    // RerunConstructionScripts on a Blueprint-instanced actor (any actor with a construction
    // script / SCS, which is most of them), and so can SaveAsset below. Reconstruction DESTROYS
    // this component (renames it TRASH_* and frees it), so any later deref of OwnerObject or
    // ValuePtr is a use-after-free. Observed: EXCEPTION_ACCESS_VIOLATION in FName::ToString while
    // building the "owner" field from a freed component (see docs/BUGS.md GAP-056).
    // TargetProp lives on the UClass (not the instance) and the actor itself survives
    // reconstruction, so those two remain valid — but the component and its value memory do not.
    FString AfterText;
    TargetProp->ExportTextItem_Direct(AfterText, ValuePtr, nullptr, nullptr, PPF_None);
    const FString OwnerName   = OwnerObject->GetName();
    // ActorName (the function param) already equals TargetActor->GetName() — TargetActor was resolved by
    // matching GetName() == ActorName above — and it's a standalone FString copy, so it stays valid across
    // the reconstruction this block guards against. (Re-deriving it here redeclared the param → C2373.)
    const FString PropTypeStr = TargetProp->GetCPPType();

    FPropertyChangedEvent ChangeEvent(TargetProp, EPropertyChangeType::ValueSet);
    OwnerObject->PostEditChangeProperty(ChangeEvent);
    // ⚠ From here on OwnerObject and ValuePtr may be dangling — do NOT dereference them again.

    if (!bConverted)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to convert 'value' for property '%s' (type=%s)"), *PropertyPath, *PropTypeStr),
            EMCPErrorCode::InvalidArgument,
            TEXT("`value` did not convert through FJsonObjectConverter::JsonValueToUProperty against the leaf property's type (listed above). Match the JSON shape to the CPP type: structs are objects keyed by UPROPERTY field name, enums are strings of the element name, object refs are asset-path strings, arrays are JSON arrays. A partial write may have occurred; re-issue with a corrected value."));
    }

    bool bSaved = false;
    if (IsValid(TargetActor)) // the actor survives component reconstruction, but guard defensively
    {
        TargetActor->MarkPackageDirty();

        // Optional persistence to the owning package (the .umap for a level actor).
        // Defaults off — the live-iteration workflow leaves the change in the editor
        // world (visible immediately) and bakes deliberately later.
        bool bSaveRequested = false;
        Params->TryGetBoolField(TEXT("save"), bSaveRequested);
        if (bSaveRequested)
        {
            if (UPackage* Pkg = TargetActor->GetPackage())
            {
                bSaved = UEditorAssetLibrary::SaveAsset(Pkg->GetName(), /*bOnlyIfIsDirty=*/false);
            }
        }
    }

    UE_LOG(LogUnrealMCP, Display,
        TEXT("actor_set_property: %s.%s  %s → %s%s"),
        *ActorName, *PropertyPath, *BeforeText, *AfterText,
        bSaved ? TEXT(" (saved)") : TEXT(""));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("name"), ActorName);
    Result->SetStringField(TEXT("property"), PropertyPath);
    Result->SetStringField(TEXT("property_type"), PropTypeStr);
    Result->SetStringField(TEXT("owner"), OwnerName);
    Result->SetStringField(TEXT("before"), BeforeText);
    Result->SetStringField(TEXT("after"), AfterText);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FMCPEditorCommands::HandleSpawnBlueprintActor(const TSharedPtr<FJsonObject>& Params)
{
    // This function will now correctly call the implementation in BlueprintCommands
    FMCPBlueprintCommands BlueprintCommands;
    return BlueprintCommands.HandleCommand(TEXT("spawn_blueprint_actor"), Params);
}

// ---------------------------------------------------------------------------
// ReadDataTable
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMCPEditorCommands::HandleReadDataTable(
    const TSharedPtr<FJsonObject>& Params)
{
    FString TablePath;
    if (!Params->TryGetStringField(TEXT("table_path"), TablePath))
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'table_path'"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`table_path` is required and must be the full asset path to a UDataTable, e.g. `/Game/Data/DT_Items`. Use `list_assets` with `asset_type='DataTable'` to discover existing data tables."));

    UDataTable* Table = LoadObject<UDataTable>(nullptr, *TablePath);
    if (!Table)
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("DataTable not found: %s"), *TablePath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`table_path` did not resolve to a UDataTable via LoadObject. Verify the path with `list_assets` (asset_type='DataTable'). Paths are case-sensitive and must include `/Game/` prefix and the .AssetName suffix is auto-handled by LoadObject."));

    const UScriptStruct* RowStruct = Table->GetRowStruct();
    if (!RowStruct)
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("DataTable has no row struct"),
            EMCPErrorCode::InvalidArgument,
            TEXT("The DataTable's RowStruct is null — typically means the table was created without a row struct or its row struct was deleted. Open the table in the editor and assign a valid UScriptStruct subclass before reading."));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("table_name"), Table->GetName());
    Result->SetStringField(TEXT("table_path"), Table->GetPathName());
    Result->SetStringField(TEXT("row_struct"), RowStruct->GetName());

    // Column definitions from the struct
    TArray<TSharedPtr<FJsonValue>> ColArr;
    for (TFieldIterator<FProperty> PropIt(RowStruct); PropIt; ++PropIt)
    {
        FProperty* Prop = *PropIt;
        TSharedPtr<FJsonObject> ColObj = MakeShared<FJsonObject>();
        ColObj->SetStringField(TEXT("name"), Prop->GetName());
        ColObj->SetStringField(TEXT("type"), Prop->GetCPPType());
        ColArr.Add(MakeShared<FJsonValueObject>(ColObj));
    }
    Result->SetArrayField(TEXT("columns"), ColArr);

    // Rows
    TArray<TSharedPtr<FJsonValue>> RowArr;
    const TMap<FName, uint8*>& RowMap = Table->GetRowMap();
    for (auto It = RowMap.CreateConstIterator(); It; ++It)
    {
        FName RowName = It.Key();
        const uint8* RowData = It.Value();
        if (!RowData) continue;

        TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
        RowObj->SetStringField(TEXT("row_name"), RowName.ToString());

        TArray<TSharedPtr<FJsonValue>> FieldArr;
        for (TFieldIterator<FProperty> PropIt(RowStruct); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            TSharedPtr<FJsonObject> FieldObj = MakeShared<FJsonObject>();
            FieldObj->SetStringField(TEXT("name"), Prop->GetName());

            FString Value;
            const uint8* ValuePtr = RowData + Prop->GetOffset_ForInternal();
            Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
            FieldObj->SetStringField(TEXT("value"), Value);
            FieldArr.Add(MakeShared<FJsonValueObject>(FieldObj));
        }
        RowObj->SetArrayField(TEXT("fields"), FieldArr);
        RowArr.Add(MakeShared<FJsonValueObject>(RowObj));
    }
    Result->SetArrayField(TEXT("rows"), RowArr);
    Result->SetNumberField(TEXT("row_count"), RowArr.Num());
    Result->SetBoolField(TEXT("success"), true);
    return Result;
}

// ---------------------------------------------------------------------------
// SetWorldGameModeOverride
// ---------------------------------------------------------------------------
// Mutates AWorldSettings::DefaultGameMode (the editor surfaces this as
// "World Settings → GameModeOverride") and saves the .umap.  Closes the
// last manual editor step in the warmup-binding chain — pairs with
// set_blueprint_default_value, which writes the soft-ref on a
// BP_<Level>_GameMode CDO; this writes the per-level pointer at it.
// Pass empty string or "None" for `gamemode_class` to clear the override.

TSharedPtr<FJsonObject> FMCPEditorCommands::HandleSetWorldGameModeOverride(
    const TSharedPtr<FJsonObject>& Params)
{
    FString LevelPath;
    if (!Params->TryGetStringField(TEXT("level_path"), LevelPath))
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'level_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`level_path` is required and must be the full asset path to a UWorld (.umap), e.g. `/Game/Levels/MainLevel`. The .AssetName suffix is appended automatically. Use `list_assets` with `asset_type='World'` to discover levels."));

    FString GameModeRef;
    if (!Params->TryGetStringField(TEXT("gamemode_class"), GameModeRef))
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'gamemode_class' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`gamemode_class` is required (string). Accepted forms: Blueprint asset path (`/Game/BPs/BP_MyGameMode` — `_C` auto-appended), full class path, `/Script/Module.Class` native path, or empty string / `None` to clear the override."));

    // Normalize "/Game/Levels/Foo" → "/Game/Levels/Foo.Foo" for LoadObject.
    FString FullLevelPath = LevelPath;
    if (!FullLevelPath.Contains(TEXT(".")))
    {
        const FString Name = FPaths::GetBaseFilename(FullLevelPath);
        FullLevelPath = FString::Printf(TEXT("%s.%s"), *FullLevelPath, *Name);
    }
    UWorld* World = LoadObject<UWorld>(nullptr, *FullLevelPath);
    if (!World)
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("World not found: %s"), *FullLevelPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("`level_path` did not resolve to a UWorld via LoadObject, even with the .AssetName suffix fallback. Verify the path with `list_assets` (asset_type='World'). Paths are case-sensitive and must include `/Game/` prefix."));

    AWorldSettings* WorldSettings = World->GetWorldSettings();
    if (!WorldSettings)
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("World has no AWorldSettings"),
            EMCPErrorCode::Internal,
            TEXT("UWorld::GetWorldSettings() returned null — every UWorld should have an AWorldSettings actor in its persistent level. This indicates the level is corrupted or loaded in an unusual state. Re-save the level via the editor and retry."));

    // Resolve the gamemode class.  Accepts a BP asset path (we append _C
    // to reach the GeneratedClass), an explicit class path, a /Script/...
    // native path, or empty/"None" to clear the override.
    UClass* GameModeClass = nullptr;
    if (!GameModeRef.IsEmpty() && GameModeRef != TEXT("None"))
    {
        if (GameModeRef.StartsWith(TEXT("/Script/")))
        {
            GameModeClass = LoadClass<AGameModeBase>(nullptr, *GameModeRef);
        }
        else
        {
            FString ClassPath = GameModeRef;
            if (!ClassPath.EndsWith(TEXT("_C")))
            {
                if (!ClassPath.Contains(TEXT(".")))
                {
                    const FString Name = FPaths::GetBaseFilename(ClassPath);
                    ClassPath = FString::Printf(TEXT("%s.%s"), *ClassPath, *Name);
                }
                ClassPath += TEXT("_C");
            }
            GameModeClass = LoadClass<AGameModeBase>(nullptr, *ClassPath);
        }

        if (!GameModeClass)
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("GameMode class not found: %s"), *GameModeRef),
                EMCPErrorCode::ClassNotLoaded,
                TEXT("`gamemode_class` did not resolve via LoadClass. For Blueprint paths, the BP must be compiled and reside at the path (the `_C` suffix is auto-appended). For native classes, use `/Script/Module.ClassName`. Use `list_assets` with `asset_type='Blueprint'` to discover BP game modes."));

        if (!GameModeClass->IsChildOf<AGameModeBase>())
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Class '%s' is not an AGameModeBase subclass"), *GameModeRef),
                EMCPErrorCode::UnsupportedClass,
                TEXT("The class resolved but does not derive from AGameModeBase. AWorldSettings::DefaultGameMode is typed `TSubclassOf<AGameModeBase>` — pass a class that derives from AGameModeBase (or AGameMode for legacy GameModes)."));
    }

    FProperty* Property = AWorldSettings::StaticClass()->FindPropertyByName(
        FName(TEXT("DefaultGameMode")));
    if (!Property)
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("AWorldSettings has no DefaultGameMode property"),
            EMCPErrorCode::Internal,
            TEXT("AWorldSettings::FindPropertyByName(`DefaultGameMode`) returned null. This is an engine-version invariant — the property has existed since UE4. If reproducible, the engine module may be partially loaded; restart the editor."));

    // Bracket the write with PreEditChange / PostEditChangeProperty so the
    // editor's transaction system + dirty-flag pipeline see the mutation —
    // same lesson as set_blueprint_default_value: bare assignment writes
    // the in-memory copy but the saved .umap drops the change.
    WorldSettings->Modify();
    WorldSettings->PreEditChange(Property);
    WorldSettings->DefaultGameMode = GameModeClass;
    FPropertyChangedEvent ChangeEvent(Property, EPropertyChangeType::ValueSet);
    WorldSettings->PostEditChangeProperty(ChangeEvent);

    World->MarkPackageDirty();
    const bool bSaved = UEditorAssetLibrary::SaveAsset(World->GetPathName(), /*bOnlyIfIsDirty=*/false);
    if (!bSaved)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to save level package: %s"), *World->GetPathName()),
            EMCPErrorCode::Internal,
            TEXT("The DefaultGameMode override was applied in memory but UEditorAssetLibrary::SaveAsset returned false — the .umap was not written to disk (checkout vetoed, package read-only, or the save was rejected). The in-memory change persists only until the editor closes; resolve source control / read-only state and re-issue, or save the level manually."));
    }

    UE_LOG(LogUnrealMCP, Display,
        TEXT("set_world_gamemode_override: %s ← %s"),
        *World->GetName(),
        GameModeClass ? *GameModeClass->GetPathName() : TEXT("None"));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("level_path"), World->GetPathName());
    Result->SetStringField(TEXT("gamemode_class"),
        GameModeClass ? GameModeClass->GetPathName() : TEXT("None"));
    return Result;
}
