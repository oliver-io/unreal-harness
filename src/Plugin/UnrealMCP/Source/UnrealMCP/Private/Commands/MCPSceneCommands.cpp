#include "Commands/MCPSceneCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "GameFramework/Actor.h"
#include "Math/Box.h"
#include "Misc/PackageName.h"

namespace
{
    UWorld* GetSceneEditorWorld()
    {
        if (!GEditor)
        {
            return nullptr;
        }
        return GEditor->GetEditorWorldContext().World();
    }

    // Resolve a UClass for actor_spawn / class-filter inputs. Delegates to the
    // shared FMCPCommonUtils::ResolveClass resolver (native paths, /Game asset
    // paths → GeneratedClass, short names, bare Blueprint names). The caller
    // enforces the AActor-subclass constraint downstream.
    UClass* ResolveActorClass(const FString& Name)
    {
        return FMCPCommonUtils::ResolveClass(Name);
    }

    FVector JsonVector(const TSharedPtr<FJsonObject>& Obj, const FString& Field, const FVector& Default = FVector::ZeroVector)
    {
        const TSharedPtr<FJsonObject>* Sub;
        if (Obj.IsValid() && Obj->TryGetObjectField(Field, Sub) && Sub->IsValid())
        {
            return FVector(
                (*Sub)->GetNumberField(TEXT("x")),
                (*Sub)->GetNumberField(TEXT("y")),
                (*Sub)->GetNumberField(TEXT("z")));
        }
        return Default;
    }

    FRotator JsonRotator(const TSharedPtr<FJsonObject>& Obj, const FString& Field, const FRotator& Default = FRotator::ZeroRotator)
    {
        const TSharedPtr<FJsonObject>* Sub;
        if (Obj.IsValid() && Obj->TryGetObjectField(Field, Sub) && Sub->IsValid())
        {
            return FRotator(
                (*Sub)->GetNumberField(TEXT("pitch")),
                (*Sub)->GetNumberField(TEXT("yaw")),
                (*Sub)->GetNumberField(TEXT("roll")));
        }
        return Default;
    }

    void WriteVector(TSharedPtr<FJsonObject>& Out, const FString& Field, const FVector& V)
    {
        TSharedPtr<FJsonObject> Sub = MakeShared<FJsonObject>();
        Sub->SetNumberField(TEXT("x"), V.X);
        Sub->SetNumberField(TEXT("y"), V.Y);
        Sub->SetNumberField(TEXT("z"), V.Z);
        Out->SetObjectField(Field, Sub);
    }

    void WriteRotator(TSharedPtr<FJsonObject>& Out, const FString& Field, const FRotator& R)
    {
        TSharedPtr<FJsonObject> Sub = MakeShared<FJsonObject>();
        Sub->SetNumberField(TEXT("pitch"), R.Pitch);
        Sub->SetNumberField(TEXT("yaw"),   R.Yaw);
        Sub->SetNumberField(TEXT("roll"),  R.Roll);
        Out->SetObjectField(Field, Sub);
    }

    TSharedPtr<FJsonObject> ActorToBriefJson(AActor* Actor)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Actor->GetName());
        Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
        Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
        Entry->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());

        const FTransform Xf = Actor->GetActorTransform();
        WriteVector(Entry, TEXT("location"), Xf.GetLocation());
        WriteRotator(Entry, TEXT("rotation"), Xf.GetRotation().Rotator());
        WriteVector(Entry, TEXT("scale"), Xf.GetScale3D());

        TArray<TSharedPtr<FJsonValue>> TagArray;
        for (const FName& Tag : Actor->Tags)
        {
            TagArray.Add(MakeShared<FJsonValueString>(Tag.ToString()));
        }
        Entry->SetArrayField(TEXT("tags"), TagArray);
        return Entry;
    }

    bool ActorMatchesAllTags(AActor* Actor, const TArray<FString>& RequiredTags)
    {
        for (const FString& Required : RequiredTags)
        {
            const FName RequiredName(*Required);
            if (!Actor->Tags.Contains(RequiredName))
            {
                return false;
            }
        }
        return true;
    }
}

TSharedPtr<FJsonObject> FMCPSceneCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("actor_query"))
    {
        return HandleActorQuery(Params);
    }
    if (CommandType == TEXT("actor_spawn"))
    {
        return HandleActorSpawn(Params);
    }
    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown scene command: %s"), *CommandType),
        EMCPErrorCode::InvalidArgument,
        TEXT("Supported scene commands in this build: actor_query, actor_spawn."));
}

TSharedPtr<FJsonObject> FMCPSceneCommands::HandleActorQuery(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetSceneEditorWorld();
    if (!World)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("No editor world available"),
            EMCPErrorCode::EngineBusy,
            TEXT("This usually means the editor is mid-load. Retry shortly."));
    }

    // ── Filter axes ─────────────────────────────────────────────────────────
    FString NamePattern;
    Params->TryGetStringField(TEXT("name_pattern"), NamePattern);

    FString ClassFilter;
    UClass* FilterClass = nullptr;
    bool bDirectOnly = false;
    if (Params->TryGetStringField(TEXT("class"), ClassFilter) && !ClassFilter.IsEmpty())
    {
        FilterClass = ResolveActorClass(ClassFilter);
        if (!FilterClass)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Class filter not found: %s"), *ClassFilter),
                EMCPErrorCode::ClassNotLoaded,
                TEXT("Pass a fully-qualified path (\"/Script/Engine.PointLight\"), an asset path (\"/Game/.../BP_Foo\" — _C will be appended), or a short name."));
        }
        Params->TryGetBoolField(TEXT("direct_only"), bDirectOnly);
    }

    // tag — accept either single string or array.
    TArray<FString> RequiredTags;
    {
        FString SingleTag;
        if (Params->TryGetStringField(TEXT("tag"), SingleTag) && !SingleTag.IsEmpty())
        {
            RequiredTags.Add(SingleTag);
        }
        else
        {
            const TArray<TSharedPtr<FJsonValue>>* TagArr;
            if (Params->TryGetArrayField(TEXT("tag"), TagArr))
            {
                for (const TSharedPtr<FJsonValue>& V : *TagArr)
                {
                    if (V.IsValid() && V->Type == EJson::String)
                    {
                        RequiredTags.Add(V->AsString());
                    }
                }
            }
        }
    }

    FString LabelFilter;
    Params->TryGetStringField(TEXT("label"), LabelFilter);

    bool bHasBBox = false;
    FBox BBox(EForceInit::ForceInit);
    {
        const TSharedPtr<FJsonObject>* BBoxObj;
        if (Params->TryGetObjectField(TEXT("bbox"), BBoxObj) && BBoxObj->IsValid())
        {
            const FVector Min = JsonVector(*BBoxObj, TEXT("min"));
            const FVector Max = JsonVector(*BBoxObj, TEXT("max"));
            BBox = FBox(Min, Max);
            bHasBBox = BBox.IsValid != 0;
        }
    }

    bool bHasRadius = false;
    FVector RadiusOrigin = FVector::ZeroVector;
    double Radius = 0.0;
    {
        const TSharedPtr<FJsonObject>* DistObj;
        if (Params->TryGetObjectField(TEXT("distance_from"), DistObj) && DistObj->IsValid())
        {
            RadiusOrigin = JsonVector(*DistObj, TEXT("origin"));
            (*DistObj)->TryGetNumberField(TEXT("radius"), Radius);
            bHasRadius = Radius > 0.0;
        }
    }

    int32 Cursor = 0;
    Params->TryGetNumberField(TEXT("cursor"), Cursor);
    if (Cursor < 0)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("'cursor' must be non-negative"),
            EMCPErrorCode::OutOfRange,
            TEXT("Pass the 'next_cursor' value from a previous response, or omit for the first page."));
    }

    int32 Limit = 200;
    Params->TryGetNumberField(TEXT("limit"), Limit);
    if (Limit <= 0 || Limit > 1000)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("'limit' must be between 1 and 1000"),
            EMCPErrorCode::OutOfRange,
            TEXT("Default limit is 200; the upper bound prevents accidentally pulling the entire scene."));
    }

    // ── Iterate already-loaded levels (no force-load) ───────────────────────
    TArray<AActor*> Matches;
    for (ULevel* Level : World->GetLevels())
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

            if (FilterClass)
            {
                const bool bClassMatch = bDirectOnly
                    ? (Actor->GetClass() == FilterClass)
                    : Actor->IsA(FilterClass);
                if (!bClassMatch) continue;
            }

            if (!NamePattern.IsEmpty())
            {
                const bool bNameHit  = Actor->GetName().Contains(NamePattern, ESearchCase::IgnoreCase);
                const bool bLabelHit = Actor->GetActorLabel().Contains(NamePattern, ESearchCase::IgnoreCase);
                if (!bNameHit && !bLabelHit) continue;
            }

            if (RequiredTags.Num() > 0 && !ActorMatchesAllTags(Actor, RequiredTags))
            {
                continue;
            }

            if (!LabelFilter.IsEmpty())
            {
                if (!Actor->GetFolderPath().ToString().Equals(LabelFilter, ESearchCase::IgnoreCase))
                {
                    continue;
                }
            }

            if (bHasBBox && !BBox.IsInsideOrOn(Actor->GetActorLocation()))
            {
                continue;
            }

            if (bHasRadius && FVector::Dist(Actor->GetActorLocation(), RadiusOrigin) > Radius)
            {
                continue;
            }

            Matches.Add(Actor);
        }
    }

    // Stable sort by path so cursor pagination is deterministic.
    Matches.Sort([](const AActor& A, const AActor& B) { return A.GetPathName() < B.GetPathName(); });

    const int32 TotalMatched = Matches.Num();
    const int32 PageEnd = FMath::Min(Cursor + Limit, TotalMatched);

    TArray<TSharedPtr<FJsonValue>> ActorArray;
    ActorArray.Reserve(FMath::Max(0, PageEnd - Cursor));
    for (int32 i = Cursor; i < PageEnd; ++i)
    {
        ActorArray.Add(MakeShared<FJsonValueObject>(ActorToBriefJson(Matches[i])));
    }

    // Skipped sublevels (parallel to scene_brief).
    TArray<TSharedPtr<FJsonValue>> SkippedSublevels;
    for (const ULevelStreaming* Streaming : World->GetStreamingLevels())
    {
        if (Streaming && !Streaming->IsLevelLoaded())
        {
            SkippedSublevels.Add(MakeShared<FJsonValueString>(Streaming->GetWorldAssetPackageName()));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("actors"), ActorArray);
    ResultObj->SetNumberField(TEXT("returned_count"), ActorArray.Num());
    ResultObj->SetNumberField(TEXT("total_matched"), TotalMatched);
    ResultObj->SetNumberField(TEXT("cursor"), Cursor);
    if (PageEnd < TotalMatched)
    {
        ResultObj->SetNumberField(TEXT("next_cursor"), PageEnd);
    }
    ResultObj->SetArrayField(TEXT("skipped_sublevels"), SkippedSublevels);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPSceneCommands::HandleActorSpawn(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = GetSceneEditorWorld();
    if (!World)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("No editor world available"),
            EMCPErrorCode::EngineBusy,
            TEXT("This usually means the editor is mid-load. Retry shortly."));
    }

    FString ClassPath;
    if (!Params->TryGetStringField(TEXT("class"), ClassPath) || ClassPath.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'class' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass a UClass path: native (\"/Script/Engine.PointLight\"), asset (\"/Game/.../BP_Foo\" — _C is appended), or short name."));
    }

    UClass* SpawnClass = ResolveActorClass(ClassPath);
    if (!SpawnClass)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class not found or could not be loaded: %s"), *ClassPath),
            EMCPErrorCode::ClassNotLoaded,
            TEXT("Verify the path and that the owning module / plugin is loaded."));
    }

    if (!SpawnClass->IsChildOf(AActor::StaticClass()))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class is not an AActor subclass: %s"), *ClassPath),
            EMCPErrorCode::UnsupportedClass,
            TEXT("actor_spawn only spawns AActor subclasses. UObjects / components / structs need different APIs."));
    }

    if (SpawnClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class is abstract and cannot be instantiated: %s"), *ClassPath),
            EMCPErrorCode::UnsupportedClass,
            TEXT("Pick a concrete subclass. Abstract classes are spawn-rejected by UE."));
    }

    const FVector Location = JsonVector(Params, TEXT("location"));
    const FRotator Rotation = JsonRotator(Params, TEXT("rotation"));
    const FVector Scale = JsonVector(Params, TEXT("scale"), FVector(1.0, 1.0, 1.0));

    FString DesiredName;
    Params->TryGetStringField(TEXT("name"), DesiredName);

    TArray<FString> TagsIn;
    {
        const TArray<TSharedPtr<FJsonValue>>* TagArr;
        if (Params->TryGetArrayField(TEXT("tags"), TagArr))
        {
            for (const TSharedPtr<FJsonValue>& V : *TagArr)
            {
                if (V.IsValid() && V->Type == EJson::String)
                {
                    TagsIn.Add(V->AsString());
                }
            }
        }
    }

    FString FolderPath;
    Params->TryGetStringField(TEXT("folder_path"), FolderPath);

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    if (!DesiredName.IsEmpty())
    {
        // FName collision is auto-disambiguated by the engine; no preflight needed.
        SpawnParams.Name = FName(*DesiredName);
        SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
    }

    // dry_run: every preflight ran above (world, class loadability, AActor
    // subclass, not abstract). Diff shape per todo/13: {actors_added:
    // [{class, location, rotation, scale, requested_name, folder_path,
    // tags}]}. The diff carries the *requested* name — UE auto-disambiguates
    // FName collisions, so the resolved name only exists after a real spawn.
    if (FMCPCommonUtils::ParseDryRun(Params))
    {
        TSharedPtr<FJsonObject> AddEntry = MakeShared<FJsonObject>();
        AddEntry->SetStringField(TEXT("class"), SpawnClass->GetPathName());
        WriteVector(AddEntry, TEXT("location"), Location);
        WriteRotator(AddEntry, TEXT("rotation"), Rotation);
        WriteVector(AddEntry, TEXT("scale"), Scale);
        if (!DesiredName.IsEmpty())  AddEntry->SetStringField(TEXT("requested_name"), DesiredName);
        if (!FolderPath.IsEmpty())   AddEntry->SetStringField(TEXT("folder_path"), FolderPath);
        if (TagsIn.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> TagArr;
            for (const FString& T : TagsIn) TagArr.Add(MakeShared<FJsonValueString>(T));
            AddEntry->SetArrayField(TEXT("tags"), TagArr);
        }

        TArray<TSharedPtr<FJsonValue>> AddedArr;
        AddedArr.Add(MakeShared<FJsonValueObject>(AddEntry));

        TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
        Diff->SetArrayField(TEXT("actors_added"), AddedArr);
        TSharedPtr<FJsonObject> Wrapped = FMCPCommonUtils::CreateDryRunResponse(Diff);
        Wrapped->SetStringField(TEXT("world"), World->GetPathName());
        return Wrapped;
    }

    AActor* Spawned = World->SpawnActor<AActor>(SpawnClass, Location, Rotation, SpawnParams);
    if (!Spawned)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("World->SpawnActor returned null for class %s"), *ClassPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UE rejected the spawn — usually a class flag (Abstract, NotPlaceable) or PIE-state mismatch. Check the editor log for the precise reason."));
    }

    Spawned->SetActorScale3D(Scale);

    // GAP-005: SpawnActor's separate-rotation argument can come back re-derived — a
    // non-trivial pitch+yaw combo emerged with the pitch MAGNITUDE itself changed
    // (e.g. -50 -> -84), not a 360-equivalent. Re-apply the rotation through the same
    // quaternion path actor_set_transform uses, which round-trips the Euler faithfully.
    Spawned->SetActorRotation(FQuat(Rotation));

    if (!DesiredName.IsEmpty())
    {
        // SetActorLabel is the editor-visible label, distinct from FName.
        Spawned->SetActorLabel(DesiredName);
    }

    for (const FString& Tag : TagsIn)
    {
        Spawned->Tags.AddUnique(FName(*Tag));
    }

    if (!FolderPath.IsEmpty())
    {
        Spawned->SetFolderPath(FName(*FolderPath));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetObjectField(TEXT("actor"), ActorToBriefJson(Spawned));
    ResultObj->SetStringField(TEXT("world"), World->GetPathName());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}
