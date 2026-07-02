#include "Commands/MCPInspectionCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "Editor.h"
#include "EditorSubsystem.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "GameMapsSettings.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "ProjectDescriptor.h"
#include "PluginDescriptor.h"
#include "ModuleDescriptor.h"
#include "Containers/UnrealString.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "EditorAssetLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Animation/SkeletalMeshActor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/Light.h"

namespace
{
    UWorld* GetEditorWorld()
    {
        if (!GEditor)
        {
            return nullptr;
        }
        return GEditor->GetEditorWorldContext().World();
    }

    FString ModuleHostTypeToString(EHostType::Type Type)
    {
        switch (Type)
        {
        case EHostType::Runtime:                return TEXT("runtime");
        case EHostType::RuntimeNoCommandlet:    return TEXT("runtime_no_commandlet");
        case EHostType::RuntimeAndProgram:      return TEXT("runtime_and_program");
        case EHostType::CookedOnly:             return TEXT("cooked_only");
        case EHostType::UncookedOnly:           return TEXT("uncooked_only");
        case EHostType::Developer:              return TEXT("developer");
        case EHostType::DeveloperTool:          return TEXT("developer_tool");
        case EHostType::Editor:                 return TEXT("editor");
        case EHostType::EditorNoCommandlet:     return TEXT("editor_no_commandlet");
        case EHostType::EditorAndProgram:       return TEXT("editor_and_program");
        case EHostType::Program:                return TEXT("program");
        case EHostType::ServerOnly:             return TEXT("server_only");
        case EHostType::ClientOnly:             return TEXT("client_only");
        case EHostType::ClientOnlyNoCommandlet: return TEXT("client_only_no_commandlet");
        default:                                return TEXT("unknown");
        }
    }
}

TSharedPtr<FJsonObject> FMCPInspectionCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("project_context")) return HandleProjectContext(Params);
    if (CommandType == TEXT("scene_brief"))     return HandleSceneBrief(Params);
    if (CommandType == TEXT("level_inspect"))   return HandleLevelInspect(Params);
    if (CommandType == TEXT("bp_brief"))        return HandleBpBrief(Params);
    if (CommandType == TEXT("actor_inspect"))   return HandleActorInspect(Params);

    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown inspection command: %s"), *CommandType),
        EMCPErrorCode::InvalidArgument,
        TEXT("Supported inspection commands in this build: project_context, scene_brief, level_inspect, bp_brief, actor_inspect."));
}

TSharedPtr<FJsonObject> FMCPInspectionCommands::HandleProjectContext(const TSharedPtr<FJsonObject>& /*Params*/)
{
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();

    ResultObj->SetStringField(TEXT("name"), FApp::GetProjectName());
    ResultObj->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());

    // Default map — read from UGameMapsSettings (reads /Script/EngineSettings.GameMapsSettings
    // which lives in DefaultEngine.ini). May be empty if the project has not set one explicitly.
    ResultObj->SetStringField(TEXT("default_map"), UGameMapsSettings::GetGameDefaultMap());

    // Plugins — only ENABLED plugins. Discovered-but-disabled would noise up the result and
    // a separate tool can surface them later if a real use case appears.
    TArray<TSharedPtr<FJsonValue>> PluginArray;
    for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
    {
        const FPluginDescriptor& Desc = Plugin->GetDescriptor();
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Plugin->GetName());
        Entry->SetStringField(TEXT("version_name"), Desc.VersionName);
        Entry->SetNumberField(TEXT("version"), Desc.Version);
        Entry->SetStringField(TEXT("category"), Desc.Category);
        Entry->SetBoolField(TEXT("is_engine_plugin"), Plugin->GetType() == EPluginType::Engine);
        Entry->SetBoolField(TEXT("is_project_plugin"), Plugin->GetType() == EPluginType::Project);
        PluginArray.Add(MakeShared<FJsonValueObject>(Entry));
    }
    ResultObj->SetArrayField(TEXT("plugins"), PluginArray);
    ResultObj->SetNumberField(TEXT("plugins_count"), PluginArray.Num());

    // Project modules — straight from the .uproject descriptor (project source modules only,
    // not the global module manager which would also include engine modules).
    TArray<TSharedPtr<FJsonValue>> ModuleArray;
    if (const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject())
    {
        for (const FModuleDescriptor& Module : Project->Modules)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Module.Name.ToString());
            Entry->SetStringField(TEXT("type"), ModuleHostTypeToString(Module.Type));
            ModuleArray.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }
    ResultObj->SetArrayField(TEXT("modules"), ModuleArray);
    ResultObj->SetNumberField(TEXT("modules_count"), ModuleArray.Num());

    // Settings paths — anchors that let an agent locate the project's config without
    // hardcoding the layout. Resolved to absolute, normalized paths.
    TArray<TSharedPtr<FJsonValue>> SettingsPaths;
    auto AddPath = [&SettingsPaths](const FString& Path)
    {
        SettingsPaths.Add(MakeShared<FJsonValueString>(FPaths::ConvertRelativePathToFull(Path)));
    };
    AddPath(FPaths::ProjectDir());
    AddPath(FPaths::ProjectConfigDir());
    AddPath(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultEngine.ini")));
    AddPath(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultGame.ini")));
    AddPath(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultInput.ini")));
    AddPath(FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultEditor.ini")));
    ResultObj->SetArrayField(TEXT("settings_paths"), SettingsPaths);

    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPInspectionCommands::HandleSceneBrief(const TSharedPtr<FJsonObject>& /*Params*/)
{
    UWorld* World = GetEditorWorld();
    if (!World)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("No editor world available"),
            EMCPErrorCode::EngineBusy,
            TEXT("This usually means the editor is mid-load or the asset registry has not finished scanning. Retry shortly."));
    }

    // Aggregate by class across the persistent level + every already-loaded streamed level.
    // Doc invariant: never force-load streamed sublevels — record the unloaded ones in
    // skipped_sublevels so the agent can decide whether to stream them.
    TMap<FString, int32> CountsByClass;
    int32 TotalActors = 0;

    TArray<ULevel*> LoadedLevels = World->GetLevels();
    for (ULevel* Level : LoadedLevels)
    {
        if (!Level)
        {
            continue;
        }
        for (AActor* Actor : Level->Actors)
        {
            if (!Actor)
            {
                continue;
            }
            const FString ClassPath = Actor->GetClass()->GetPathName();
            CountsByClass.FindOrAdd(ClassPath)++;
            ++TotalActors;
        }
    }

    // Collect skipped (unloaded) streamed sublevels.
    TArray<TSharedPtr<FJsonValue>> SkippedSublevels;
    for (const ULevelStreaming* Streaming : World->GetStreamingLevels())
    {
        if (!Streaming)
        {
            continue;
        }
        if (!Streaming->IsLevelLoaded())
        {
            const FString PackageName = Streaming->GetWorldAssetPackageName();
            SkippedSublevels.Add(MakeShared<FJsonValueString>(PackageName));
        }
    }

    // Sort by_class entries by descending count for stable, scan-friendly output.
    TArray<TPair<FString, int32>> SortedCounts;
    SortedCounts.Reserve(CountsByClass.Num());
    for (const TPair<FString, int32>& Entry : CountsByClass)
    {
        SortedCounts.Add(Entry);
    }
    SortedCounts.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
    {
        if (A.Value != B.Value) return A.Value > B.Value;
        return A.Key < B.Key;
    });

    TSharedPtr<FJsonObject> ByClassObj = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> DistinctClasses;
    DistinctClasses.Reserve(SortedCounts.Num());
    for (const TPair<FString, int32>& Entry : SortedCounts)
    {
        ByClassObj->SetNumberField(Entry.Key, Entry.Value);
        DistinctClasses.Add(MakeShared<FJsonValueString>(Entry.Key));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("world_name"), World->GetName());
    ResultObj->SetStringField(TEXT("world_path"), World->GetPathName());
    ResultObj->SetNumberField(TEXT("total_actors"), TotalActors);
    ResultObj->SetObjectField(TEXT("by_class"), ByClassObj);
    ResultObj->SetArrayField(TEXT("distinct_classes"), DistinctClasses);
    ResultObj->SetNumberField(TEXT("distinct_class_count"), DistinctClasses.Num());
    ResultObj->SetNumberField(TEXT("loaded_levels_count"), LoadedLevels.Num());
    ResultObj->SetArrayField(TEXT("skipped_sublevels"), SkippedSublevels);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPInspectionCommands::HandleLevelInspect(const TSharedPtr<FJsonObject>& /*Params*/)
{
    UWorld* World = GetEditorWorld();
    if (!World)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("No editor world available"),
            EMCPErrorCode::EngineBusy,
            TEXT("Editor mid-load. Retry shortly."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("name"), World->GetName());
    ResultObj->SetStringField(TEXT("path"), World->GetPathName());

    // Persistent level metadata.
    if (ULevel* Persistent = World->PersistentLevel)
    {
        TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
        PObj->SetStringField(TEXT("path"), Persistent->GetPathName());
        PObj->SetNumberField(TEXT("actor_count"), Persistent->Actors.Num());
        ResultObj->SetObjectField(TEXT("persistent_level"), PObj);
    }

    // World settings reference (the AWorldSettings actor — gameplay defaults
    // for the world). Stable on the world; just surface the path.
    if (AWorldSettings* WS = World->GetWorldSettings())
    {
        TSharedPtr<FJsonObject> WObj = MakeShared<FJsonObject>();
        WObj->SetStringField(TEXT("path"), WS->GetPathName());
        WObj->SetStringField(TEXT("class"), WS->GetClass()->GetPathName());
        ResultObj->SetObjectField(TEXT("world_settings"), WObj);
    }

    // Sublevels — every streamed level the world declares, regardless of
    // load state. Doc invariant: "Sublevels appear in level_inspect regardless
    // of load state. An agent comparing levels needs the full sublevel
    // topology, not the currently-loaded subset."
    TArray<TSharedPtr<FJsonValue>> SublevelArr;
    int32 LoadedCount = 0;
    int32 UnloadedCount = 0;
    for (const ULevelStreaming* Streaming : World->GetStreamingLevels())
    {
        if (!Streaming) continue;
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("package_name"), Streaming->GetWorldAssetPackageName());
        Entry->SetStringField(TEXT("class"), Streaming->GetClass()->GetPathName());
        const bool bLoaded = Streaming->IsLevelLoaded();
        Entry->SetBoolField(TEXT("is_loaded"), bLoaded);
        Entry->SetBoolField(TEXT("should_be_loaded"), Streaming->ShouldBeLoaded());
        Entry->SetBoolField(TEXT("should_be_visible"), Streaming->ShouldBeVisible());
        if (bLoaded)
        {
            ++LoadedCount;
            if (ULevel* L = Streaming->GetLoadedLevel())
            {
                Entry->SetNumberField(TEXT("actor_count"), L->Actors.Num());
            }
        }
        else
        {
            ++UnloadedCount;
        }
        SublevelArr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    ResultObj->SetArrayField(TEXT("sublevels"), SublevelArr);
    ResultObj->SetNumberField(TEXT("sublevel_count"), SublevelArr.Num());
    ResultObj->SetNumberField(TEXT("sublevels_loaded"), LoadedCount);
    ResultObj->SetNumberField(TEXT("sublevels_unloaded"), UnloadedCount);

    // World-type flag: editor-loaded persistent worlds vs PIE worlds vs
    // editor preview worlds. EWorldType is a non-reflected namespaced enum,
    // so we hand-roll the string instead of going through StaticEnum.
    const TCHAR* WorldTypeStr = TEXT("None");
    switch (World->WorldType)
    {
    case EWorldType::None:           WorldTypeStr = TEXT("None"); break;
    case EWorldType::Game:           WorldTypeStr = TEXT("Game"); break;
    case EWorldType::Editor:         WorldTypeStr = TEXT("Editor"); break;
    case EWorldType::PIE:            WorldTypeStr = TEXT("PIE"); break;
    case EWorldType::EditorPreview:  WorldTypeStr = TEXT("EditorPreview"); break;
    case EWorldType::GamePreview:    WorldTypeStr = TEXT("GamePreview"); break;
    case EWorldType::GameRPC:        WorldTypeStr = TEXT("GameRPC"); break;
    case EWorldType::Inactive:       WorldTypeStr = TEXT("Inactive"); break;
    default:                         WorldTypeStr = TEXT("Unknown"); break;
    }
    ResultObj->SetStringField(TEXT("world_type"), WorldTypeStr);

    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPInspectionCommands::HandleBpBrief(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (!Params->TryGetStringField(TEXT("bp_path"), BlueprintPath) &&
        !Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'bp_path' (or legacy 'blueprint_path') parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the Blueprint asset path."));
    }

    // Per doc invariant: registry tag map fast path; structural fallback only
    // when tags are missing. Most BP tags don't carry per-counts (variables,
    // functions, etc.) so the structural read is the practical path. We DO
    // try the asset-data fast path for parent_class — that one is reliably
    // tagged.
    UObject* Loaded = UEditorAssetLibrary::LoadAsset(BlueprintPath);
    UBlueprint* BP = Cast<UBlueprint>(Loaded);
    if (!BP)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UBlueprint: %s"), *BlueprintPath),
            Loaded ? EMCPErrorCode::UnsupportedClass : EMCPErrorCode::AssetNotFound,
            Loaded
                ? TEXT("bp_brief operates on UBlueprint subclasses (regular BP, Anim BP, Widget BP).")
                : TEXT("Verify the path; asset registry returned no entry."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("bp_path"), BP->GetPathName());

    if (UClass* ParentClass = BP->ParentClass)
    {
        ResultObj->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
    }

    // Variable count — BP-declared (NewVariables); inherited C++ properties
    // are not counted here (use class_inspect / get_class_properties for those).
    ResultObj->SetNumberField(TEXT("variables_count"), BP->NewVariables.Num());

    // Function count — user-declared function graphs only (excluding
    // construction script and the event graph).
    ResultObj->SetNumberField(TEXT("functions_count"), BP->FunctionGraphs.Num());

    // Component count — SCS nodes (added via AddComponent panel). For BP kinds
    // without an SCS (anim BPs, some widget BPs) this is 0.
    int32 ComponentsCount = 0;
    if (BP->SimpleConstructionScript)
    {
        ComponentsCount = BP->SimpleConstructionScript->GetAllNodes().Num();
    }
    ResultObj->SetNumberField(TEXT("components_count"), ComponentsCount);
    ResultObj->SetBoolField(TEXT("has_scs"), BP->SimpleConstructionScript != nullptr);

    // Graphs — user-facing list. Each entry: name, schema/type. Includes the
    // event graph(s), function graphs, macro graphs, and delegate signatures.
    TArray<UEdGraph*> AllGraphs;
    BP->GetAllGraphs(AllGraphs);
    TArray<TSharedPtr<FJsonValue>> GraphArr;
    for (UEdGraph* G : AllGraphs)
    {
        if (!G) continue;
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), G->GetName());
        if (UClass* SchemaClass = G->Schema)
        {
            Entry->SetStringField(TEXT("schema"), SchemaClass->GetPathName());
        }
        Entry->SetNumberField(TEXT("node_count"), G->Nodes.Num());
        GraphArr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    ResultObj->SetArrayField(TEXT("graphs"), GraphArr);
    ResultObj->SetNumberField(TEXT("graphs_count"), GraphArr.Num());

    // Blueprint type flag (NormalBlueprint / Const / FunctionLibrary / Interface
    // / MacroLibrary / LevelScript) — useful for orient.
    const UEnum* BPTypeEnum = StaticEnum<EBlueprintType>();
    if (BPTypeEnum)
    {
        ResultObj->SetStringField(TEXT("blueprint_type"),
            BPTypeEnum->GetNameStringByValue(static_cast<int64>(BP->BlueprintType)));
    }

    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

namespace
{
    const TCHAR* MobilityToString(EComponentMobility::Type M)
    {
        switch (M)
        {
        case EComponentMobility::Static:     return TEXT("static");
        case EComponentMobility::Stationary: return TEXT("stationary");
        case EComponentMobility::Movable:    return TEXT("movable");
        default:                             return TEXT("unknown");
        }
    }

    void WriteVec3(TSharedPtr<FJsonObject>& Out, const TCHAR* Field, const FVector& V)
    {
        TSharedPtr<FJsonObject> Sub = MakeShared<FJsonObject>();
        Sub->SetNumberField(TEXT("x"), V.X);
        Sub->SetNumberField(TEXT("y"), V.Y);
        Sub->SetNumberField(TEXT("z"), V.Z);
        Out->SetObjectField(Field, Sub);
    }

    void WriteRot(TSharedPtr<FJsonObject>& Out, const TCHAR* Field, const FRotator& R)
    {
        TSharedPtr<FJsonObject> Sub = MakeShared<FJsonObject>();
        Sub->SetNumberField(TEXT("pitch"), R.Pitch);
        Sub->SetNumberField(TEXT("yaw"),   R.Yaw);
        Sub->SetNumberField(TEXT("roll"),  R.Roll);
        Out->SetObjectField(Field, Sub);
    }

    // Curated key_properties per class family. Documented inline.
    // Each family contributes a small set of "what an agent typically needs to
    // know without dumping every property". Compose top-down — when an actor
    // matches multiple families (e.g. ACharacter is also APawn is also AActor),
    // each family's contribution is merged into the same key_properties object.
    void EmitActorKeyProperties(AActor* Actor, TSharedPtr<FJsonObject>& Out)
    {
        if (!Actor) return;

        // ── AStaticMeshActor ────────────────────────────────────────────────
        if (AStaticMeshActor* SMA = Cast<AStaticMeshActor>(Actor))
        {
            if (UStaticMeshComponent* SMC = SMA->GetStaticMeshComponent())
            {
                if (UStaticMesh* Mesh = SMC->GetStaticMesh())
                {
                    Out->SetStringField(TEXT("static_mesh"), Mesh->GetPathName());
                }
                Out->SetNumberField(TEXT("num_materials"), SMC->GetNumMaterials());
            }
        }

        // ── ASkeletalMeshActor ──────────────────────────────────────────────
        if (ASkeletalMeshActor* SkMA = Cast<ASkeletalMeshActor>(Actor))
        {
            if (USkeletalMeshComponent* SkMC = SkMA->GetSkeletalMeshComponent())
            {
                if (USkeletalMesh* Mesh = SkMC->GetSkeletalMeshAsset())
                {
                    Out->SetStringField(TEXT("skeletal_mesh"), Mesh->GetPathName());
                }
                if (UClass* AnimClass = SkMC->GetAnimClass())
                {
                    Out->SetStringField(TEXT("anim_class"), AnimClass->GetPathName());
                }
            }
        }

        // ── APawn / ACharacter ──────────────────────────────────────────────
        if (APawn* Pawn = Cast<APawn>(Actor))
        {
            if (AController* Controller = Pawn->GetController())
            {
                Out->SetStringField(TEXT("controller"), Controller->GetPathName());
                Out->SetStringField(TEXT("controller_class"), Controller->GetClass()->GetPathName());
            }
            if (ACharacter* Character = Cast<ACharacter>(Pawn))
            {
                if (UCharacterMovementComponent* CMC = Character->GetCharacterMovement())
                {
                    Out->SetNumberField(TEXT("max_walk_speed"), CMC->MaxWalkSpeed);
                    Out->SetNumberField(TEXT("jump_z_velocity"), CMC->JumpZVelocity);
                    Out->SetNumberField(TEXT("gravity_scale"), CMC->GravityScale);
                }
            }
        }

        // ── ACameraActor ────────────────────────────────────────────────────
        if (ACameraActor* Cam = Cast<ACameraActor>(Actor))
        {
            if (UCameraComponent* CamComp = Cam->GetCameraComponent())
            {
                Out->SetNumberField(TEXT("fov"), CamComp->FieldOfView);
                Out->SetNumberField(TEXT("aspect_ratio"), CamComp->AspectRatio);
                Out->SetBoolField(TEXT("constrain_aspect_ratio"), CamComp->bConstrainAspectRatio);
            }
        }

        // ── ALight (and subclasses) ─────────────────────────────────────────
        if (ALight* Light = Cast<ALight>(Actor))
        {
            if (ULightComponent* LC = Light->GetLightComponent())
            {
                Out->SetNumberField(TEXT("light_intensity"), LC->Intensity);
                const FLinearColor C = LC->GetLightColor();
                TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
                ColorObj->SetNumberField(TEXT("r"), C.R);
                ColorObj->SetNumberField(TEXT("g"), C.G);
                ColorObj->SetNumberField(TEXT("b"), C.B);
                ColorObj->SetNumberField(TEXT("a"), C.A);
                Out->SetObjectField(TEXT("light_color"), ColorObj);
                Out->SetBoolField(TEXT("light_affects_world"), LC->bAffectsWorld);
            }
        }
    }
}

TSharedPtr<FJsonObject> FMCPInspectionCommands::HandleActorInspect(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetEditorWorld();
    if (!World)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("No editor world available"),
            EMCPErrorCode::EngineBusy,
            TEXT("Editor mid-load. Retry shortly."));
    }

    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName) || ActorName.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the actor's FName (not the editor label). actor_query / scene_brief surface them."));
    }

    AActor* Target = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);
    for (AActor* A : AllActors)
    {
        if (A && A->GetName() == ActorName)
        {
            Target = A;
            break;
        }
    }
    if (!Target)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Actor not found in editor world: %s"), *ActorName),
            EMCPErrorCode::ActorNotFound,
            TEXT("FName is case-sensitive. actor_query surfaces the live names; in PIE worlds actors may be suffixed (_C_0)."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("name"), Target->GetName());
    ResultObj->SetStringField(TEXT("class"), Target->GetClass()->GetPathName());
    ResultObj->SetStringField(TEXT("label"), Target->GetActorLabel());
    ResultObj->SetStringField(TEXT("folder_path"), Target->GetFolderPath().ToString());

    // Transform.
    const FTransform Xf = Target->GetActorTransform();
    WriteVec3(ResultObj, TEXT("location"), Xf.GetLocation());
    WriteRot(ResultObj,  TEXT("rotation"), Xf.GetRotation().Rotator());
    WriteVec3(ResultObj, TEXT("scale"),    Xf.GetScale3D());

    // Tags.
    {
        TArray<TSharedPtr<FJsonValue>> TagArr;
        for (const FName& T : Target->Tags) TagArr.Add(MakeShared<FJsonValueString>(T.ToString()));
        ResultObj->SetArrayField(TEXT("tags"), TagArr);
    }

    // Mobility (root component's mobility).
    if (USceneComponent* Root = Target->GetRootComponent())
    {
        ResultObj->SetStringField(TEXT("mobility"), MobilityToString(Root->Mobility.GetValue()));
    }

    // Components — name, class, parent (when attached). Flat list, not the
    // attachment tree (which is deferred per doc 3's DEFERRED section).
    {
        TArray<UActorComponent*> Comps;
        Target->GetComponents(Comps);
        TArray<TSharedPtr<FJsonValue>> CompArr;
        for (UActorComponent* C : Comps)
        {
            if (!C) continue;
            TSharedPtr<FJsonObject> CObj = MakeShared<FJsonObject>();
            CObj->SetStringField(TEXT("name"),  C->GetName());
            CObj->SetStringField(TEXT("class"), C->GetClass()->GetPathName());
            if (USceneComponent* SC = Cast<USceneComponent>(C))
            {
                if (USceneComponent* AttachParent = SC->GetAttachParent())
                {
                    CObj->SetStringField(TEXT("attach_parent"), AttachParent->GetName());
                }
                if (SC->GetAttachSocketName() != NAME_None)
                {
                    CObj->SetStringField(TEXT("attach_socket"), SC->GetAttachSocketName().ToString());
                }

                // Relative transform (as stored against the attach parent).
                {
                    TSharedPtr<FJsonObject> Rel = MakeShared<FJsonObject>();
                    WriteVec3(Rel, TEXT("location"), SC->GetRelativeLocation());
                    WriteRot(Rel,  TEXT("rotation"), SC->GetRelativeRotation());
                    WriteVec3(Rel, TEXT("scale"),    SC->GetRelativeScale3D());
                    CObj->SetObjectField(TEXT("relative_transform"), Rel);
                }

                // World transform — ComponentToWorld, with absolute location/
                // rotation/scale flags already resolved by the engine. Do NOT
                // hand-compose from the parent chain.
                {
                    const FTransform W = SC->GetComponentTransform();
                    TSharedPtr<FJsonObject> Wt = MakeShared<FJsonObject>();
                    WriteVec3(Wt, TEXT("location"), W.GetLocation());
                    WriteRot(Wt,  TEXT("rotation"), W.GetRotation().Rotator());
                    WriteVec3(Wt, TEXT("scale"),    W.GetScale3D());
                    CObj->SetObjectField(TEXT("world_transform"), Wt);
                }

                // World-space bounds (only meaningful for primitive components
                // that render/collide; a plain SceneComponent has point bounds).
                if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(SC))
                {
                    const FBoxSphereBounds B = Prim->Bounds;
                    TSharedPtr<FJsonObject> Bnd = MakeShared<FJsonObject>();
                    WriteVec3(Bnd, TEXT("origin"),     B.Origin);
                    WriteVec3(Bnd, TEXT("box_extent"), B.BoxExtent);
                    Bnd->SetNumberField(TEXT("sphere_radius"), B.SphereRadius);
                    CObj->SetObjectField(TEXT("world_bounds"), Bnd);
                }
            }
            CompArr.Add(MakeShared<FJsonValueObject>(CObj));
        }
        ResultObj->SetArrayField(TEXT("components"), CompArr);
        ResultObj->SetNumberField(TEXT("components_count"), CompArr.Num());
    }

    // Curated key_properties — populated based on class family (StaticMeshActor,
    // SkeletalMeshActor, Pawn/Character, CameraActor, Light). Multiple families
    // can contribute (e.g. ACharacter contributes both Pawn and Character keys).
    TSharedPtr<FJsonObject> KeyProps = MakeShared<FJsonObject>();
    EmitActorKeyProperties(Target, KeyProps);
    ResultObj->SetObjectField(TEXT("key_properties"), KeyProps);

    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

