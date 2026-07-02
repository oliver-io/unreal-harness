#include "Commands/MCPGameplayTagCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "GameplayTagsManager.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagContainer.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

namespace
{
    bool IsValidTagPath(const FString& Tag, FString& OutError)
    {
        if (Tag.IsEmpty())
        {
            OutError = TEXT("Tag is empty.");
            return false;
        }
        if (Tag.Len() >= NAME_SIZE)
        {
            OutError = FString::Printf(TEXT("Tag is too long (%d chars; max %d)."), Tag.Len(), NAME_SIZE - 1);
            return false;
        }
        if (Tag.Len() >= NAME_SIZE)
        {
            OutError = FString::Printf(TEXT("Tag is too long (%d chars; max %d)."), Tag.Len(), NAME_SIZE - 1);
            return false;
        }
        if (Tag.StartsWith(TEXT(".")) || Tag.EndsWith(TEXT(".")))
        {
            OutError = TEXT("Tag must not start or end with a dot.");
            return false;
        }
        if (Tag.Contains(TEXT("..")))
        {
            OutError = TEXT("Tag must not contain empty segments (consecutive dots).");
            return false;
        }
        // Reject obvious illegals beyond the dot. UE accepts a fairly broad character set
        // in tag names; we trust the editor module's deeper validation but flag the easy ones.
        const TCHAR* Invalid = TEXT(" /\\:\"*?<>|");
        for (int32 i = 0; Invalid[i] != TEXT('\0'); ++i)
        {
            const TCHAR Ch = Invalid[i];
            for (int32 j = 0; j < Tag.Len(); ++j)
            {
                if (Tag[j] == Ch)
                {
                    OutError = FString::Printf(TEXT("Tag contains invalid character '%c': %s"), Ch, *Tag);
                    return false;
                }
            }
        }
        return true;
    }

    // Find assets that reference the given tag through the SearchableName dependency channel.
    // Tags appear in the registry as FAssetIdentifier(FGameplayTag::StaticStruct(), TagName).
    void GatherTagReferencers(const FName TagName, TArray<FString>& OutPackageNames)
    {
        FAssetRegistryModule& RegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& Registry = RegistryModule.Get();

        const FAssetIdentifier TagId(FGameplayTag::StaticStruct(), TagName);
        TArray<FAssetIdentifier> Referencers;
        Registry.GetReferencers(TagId, Referencers, UE::AssetRegistry::EDependencyCategory::SearchableName);

        for (const FAssetIdentifier& Ref : Referencers)
        {
            if (!Ref.PackageName.IsNone())
            {
                OutPackageNames.AddUnique(Ref.PackageName.ToString());
            }
        }
    }
}

TSharedPtr<FJsonObject> FMCPGameplayTagCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("tag_add"))    return HandleTagAdd(Params);
    if (CommandType == TEXT("tag_remove")) return HandleTagRemove(Params);
    if (CommandType == TEXT("tag_list"))   return HandleTagList(Params);
    if (CommandType == TEXT("tag_move"))   return HandleTagMove(Params);

    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown gameplay tag command: %s"), *CommandType),
        EMCPErrorCode::InvalidArgument,
        TEXT("Supported gameplay tag commands: tag_add, tag_remove, tag_list, tag_move."));
}

TSharedPtr<FJsonObject> FMCPGameplayTagCommands::HandleTagAdd(const TSharedPtr<FJsonObject>& Params)
{
    FString Tag;
    if (!Params->TryGetStringField(TEXT("tag"), Tag))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'tag' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass a dotted gameplay tag path, e.g. \"Combat.Damage.Fire\"."));
    }

    FString PathError;
    if (!IsValidTagPath(Tag, PathError))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            PathError,
            EMCPErrorCode::InvalidArgument,
            TEXT("Tag paths are dot-separated; no leading/trailing dots, no empty segments, no spaces or filesystem-illegal characters."));
    }

    FString DevComment;
    Params->TryGetStringField(TEXT("dev_comment"), DevComment);

    FString SourceName;
    Params->TryGetStringField(TEXT("source"), SourceName);
    if (SourceName.Len() >= NAME_SIZE)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("source is too long (%d chars; max %d)"), SourceName.Len(), NAME_SIZE - 1),
            EMCPErrorCode::InvalidArgument,
            TEXT("Tag source names are bounded by FName's max length."));
    }
    const FName TagSourceName = SourceName.IsEmpty() ? NAME_None : FName(*SourceName);

    if (!IGameplayTagsEditorModule::IsAvailable())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("GameplayTagsEditor module is not loaded"),
            EMCPErrorCode::FeatureDisabled,
            TEXT("The GameplayTagsEditor plugin is required for INI writeback. It is enabled by default in UE; verify your project hasn't disabled it."));
    }

    // dry_run: every preflight (path validity, module availability) ran above.
    // Diff shape per todo/13: {added: [{tag, dev_comment, source}]}.
    if (FMCPCommonUtils::ParseDryRun(Params))
    {
        TSharedPtr<FJsonObject> AddEntry = MakeShared<FJsonObject>();
        AddEntry->SetStringField(TEXT("tag"), Tag);
        AddEntry->SetStringField(TEXT("dev_comment"), DevComment);
        AddEntry->SetStringField(TEXT("source"), TagSourceName.IsNone() ? TEXT("DefaultGameplayTags.ini") : TagSourceName.ToString());

        TArray<TSharedPtr<FJsonValue>> AddedArr;
        AddedArr.Add(MakeShared<FJsonValueObject>(AddEntry));

        TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
        Diff->SetArrayField(TEXT("added"), AddedArr);
        return FMCPCommonUtils::CreateDryRunResponse(Diff);
    }

    const bool bAdded = IGameplayTagsEditorModule::Get().AddNewGameplayTagToINI(
        Tag, DevComment, TagSourceName, /*bIsRestrictedTag=*/false, /*bAllowNonRestrictedChildren=*/true);
    if (!bAdded)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("AddNewGameplayTagToINI refused tag: %s"), *Tag),
            EMCPErrorCode::Internal,
            TEXT("UE rejected the add — usually because the tag already exists, or the source name is invalid. Check the editor log."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("tag"), Tag);
    ResultObj->SetStringField(TEXT("dev_comment"), DevComment);
    ResultObj->SetStringField(TEXT("source"), TagSourceName.IsNone() ? TEXT("DefaultGameplayTags.ini") : TagSourceName.ToString());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPGameplayTagCommands::HandleTagRemove(const TSharedPtr<FJsonObject>& Params)
{
    FString Tag;
    if (!Params->TryGetStringField(TEXT("tag"), Tag))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'tag' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the dotted tag path to remove."));
    }

    bool bForce = false;
    Params->TryGetBoolField(TEXT("force"), bForce);

    if (Tag.Len() >= NAME_SIZE)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Tag is too long (%d chars; max %d)"), Tag.Len(), NAME_SIZE - 1),
            EMCPErrorCode::InvalidArgument,
            TEXT("Gameplay tag names are bounded by FName's max length."));
    }

    UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
    TSharedPtr<FGameplayTagNode> Node = Manager.FindTagNode(FName(*Tag));
    if (!Node.IsValid())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Tag not found: %s"), *Tag),
            EMCPErrorCode::UnknownTag,
            TEXT("Verify the tag exists. tag_list with prefix=<head> shows the registry."));
    }

    // Reference check (asset registry SearchableName dependencies on this tag).
    TArray<FString> Referencers;
    GatherTagReferencers(FName(*Tag), Referencers);

    if (Referencers.Num() > 0 && !bForce)
    {
        TSharedPtr<FJsonObject> Err = FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Tag '%s' is referenced by %d asset(s); refused"), *Tag, Referencers.Num()),
            EMCPErrorCode::WouldBreakReferences,
            TEXT("Pass force=true to remove anyway, or redirect the references first. The full referencer list is on result.referencers."));

        TArray<TSharedPtr<FJsonValue>> RefArr;
        for (const FString& R : Referencers)
        {
            RefArr.Add(MakeShared<FJsonValueString>(R));
        }
        Err->SetArrayField(TEXT("referencers"), RefArr);
        Err->SetNumberField(TEXT("referencer_count"), Referencers.Num());
        return Err;
    }

    if (!IGameplayTagsEditorModule::IsAvailable())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("GameplayTagsEditor module is not loaded"),
            EMCPErrorCode::FeatureDisabled,
            TEXT("The GameplayTagsEditor plugin is required for INI writeback."));
    }

    // dry_run: validation parity already enforced — would_break_references
    // returned above for the !force+refs case, exactly as commit would fail.
    // Past that point we surface the diff: {removed: [{tag}], references_affected: [...]}.
    // This is the agent's "preview the destruction" path — especially useful
    // when force=true so they see exactly which assets get orphaned.
    if (FMCPCommonUtils::ParseDryRun(Params))
    {
        TSharedPtr<FJsonObject> RemoveEntry = MakeShared<FJsonObject>();
        RemoveEntry->SetStringField(TEXT("tag"), Tag);

        TArray<TSharedPtr<FJsonValue>> RemovedArr;
        RemovedArr.Add(MakeShared<FJsonValueObject>(RemoveEntry));

        TArray<TSharedPtr<FJsonValue>> RefArr;
        for (const FString& R : Referencers)
        {
            RefArr.Add(MakeShared<FJsonValueString>(R));
        }

        TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
        Diff->SetArrayField(TEXT("removed"), RemovedArr);
        Diff->SetArrayField(TEXT("references_affected"), RefArr);
        TSharedPtr<FJsonObject> Wrapped = FMCPCommonUtils::CreateDryRunResponse(Diff);
        Wrapped->SetNumberField(TEXT("referencer_count"), Referencers.Num());
        Wrapped->SetBoolField(TEXT("force"), bForce);
        return Wrapped;
    }

    const bool bDeleted = IGameplayTagsEditorModule::Get().DeleteTagFromINI(Node);
    if (!bDeleted)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("DeleteTagFromINI refused tag: %s"), *Tag),
            EMCPErrorCode::Internal,
            TEXT("UE rejected the delete — usually because the tag's source is read-only or has children blocking removal. Check the editor log."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("tag"), Tag);
    ResultObj->SetBoolField(TEXT("force"), bForce);
    ResultObj->SetNumberField(TEXT("referencer_count"), Referencers.Num());
    {
        TArray<TSharedPtr<FJsonValue>> RefArr;
        for (const FString& R : Referencers)
        {
            RefArr.Add(MakeShared<FJsonValueString>(R));
        }
        ResultObj->SetArrayField(TEXT("referencers"), RefArr);
    }
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPGameplayTagCommands::HandleTagList(const TSharedPtr<FJsonObject>& Params)
{
    FString Prefix;
    Params->TryGetStringField(TEXT("prefix"), Prefix);

    bool bIncludeDevComments = true;
    Params->TryGetBoolField(TEXT("include_dev_comments"), bIncludeDevComments);

    UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
    FGameplayTagContainer AllTags;
    Manager.RequestAllGameplayTags(AllTags, /*OnlyIncludeDictionaryTags=*/true);

    TArray<TSharedPtr<FJsonValue>> TagArr;
    for (auto It = AllTags.CreateConstIterator(); It; ++It)
    {
        const FGameplayTag& T = *It;
        const FString Name = T.ToString();
        if (!Prefix.IsEmpty() && !Name.StartsWith(Prefix, ESearchCase::IgnoreCase))
        {
            continue;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("tag"), Name);

        if (bIncludeDevComments)
        {
            // Dev-comment + source live on the FGameplayTagNode.
            if (TSharedPtr<FGameplayTagNode> Node = Manager.FindTagNode(T))
            {
                Entry->SetStringField(TEXT("dev_comment"), Node->GetDevComment());
            }
        }

        TagArr.Add(MakeShared<FJsonValueObject>(Entry));
    }

    // Stable alphabetical sort for deterministic output.
    TagArr.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
    {
        return A->AsObject()->GetStringField(TEXT("tag")) < B->AsObject()->GetStringField(TEXT("tag"));
    });

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("prefix"), Prefix);
    ResultObj->SetArrayField(TEXT("tags"), TagArr);
    ResultObj->SetNumberField(TEXT("count"), TagArr.Num());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPGameplayTagCommands::HandleTagMove(const TSharedPtr<FJsonObject>& Params)
{
    FString FromTag, ToTag;
    if (!Params->TryGetStringField(TEXT("from"), FromTag) ||
        !Params->TryGetStringField(TEXT("to"), ToTag))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'from' or 'to' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass both 'from' (existing tag) and 'to' (new tag path)."));
    }

    FString PathErr;
    if (!IsValidTagPath(FromTag, PathErr))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Invalid 'from' tag: %s"), *PathErr),
            EMCPErrorCode::InvalidArgument,
            TEXT("Tag paths are dot-separated; no leading/trailing dots, no empty segments."));
    }
    if (!IsValidTagPath(ToTag, PathErr))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Invalid 'to' tag: %s"), *PathErr),
            EMCPErrorCode::InvalidArgument,
            TEXT("Tag paths are dot-separated; no leading/trailing dots, no empty segments."));
    }

    if (!IGameplayTagsEditorModule::IsAvailable())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("GameplayTagsEditor module is not loaded"),
            EMCPErrorCode::FeatureDisabled,
            TEXT("The GameplayTagsEditor plugin is required for INI writeback."));
    }

    UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
    if (!Manager.FindTagNode(FName(*FromTag)).IsValid())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Source tag does not exist: %s"), *FromTag),
            EMCPErrorCode::UnknownTag,
            TEXT("Verify the tag exists in the registry. tag_list with a matching prefix shows what's there."));
    }

    bool bRenameChildren = true;
    Params->TryGetBoolField(TEXT("rename_children"), bRenameChildren);

    // dry_run: validation already ran (path validity, module availability,
    // source-exists). The redirector behavior is unconditional in UE's API,
    // so the diff carries `redirector_written: true` to match what commit does.
    // references_affected lists assets currently referencing the old tag —
    // those references continue to resolve through the redirector after a
    // real commit, so the list is informational rather than a "this will
    // break" warning.
    if (FMCPCommonUtils::ParseDryRun(Params))
    {
        TSharedPtr<FJsonObject> MoveEntry = MakeShared<FJsonObject>();
        MoveEntry->SetStringField(TEXT("from"), FromTag);
        MoveEntry->SetStringField(TEXT("to"),   ToTag);

        TArray<TSharedPtr<FJsonValue>> MovedArr;
        MovedArr.Add(MakeShared<FJsonValueObject>(MoveEntry));

        TArray<FString> Referencers;
        GatherTagReferencers(FName(*FromTag), Referencers);
        TArray<TSharedPtr<FJsonValue>> RefArr;
        for (const FString& R : Referencers)
        {
            RefArr.Add(MakeShared<FJsonValueString>(R));
        }

        TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
        Diff->SetArrayField(TEXT("moved"), MovedArr);
        Diff->SetArrayField(TEXT("references_affected"), RefArr);
        TSharedPtr<FJsonObject> Wrapped = FMCPCommonUtils::CreateDryRunResponse(Diff);
        Wrapped->SetBoolField(TEXT("rename_children"), bRenameChildren);
        Wrapped->SetBoolField(TEXT("redirector_written"), true);
        Wrapped->SetNumberField(TEXT("referencer_count"), Referencers.Num());
        return Wrapped;
    }

    // RenameTagInINI leaves a redirector in the INI. The doc invariant is "rename-with-redirector
    // by default"; a `redirect=false` opt-out is not exposed by UE's editor module API in 5.7,
    // so we honor the default behavior and document that the redirector is always written.
    const bool bRenamed = IGameplayTagsEditorModule::Get().RenameTagInINI(FromTag, ToTag, bRenameChildren);
    if (!bRenamed)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("RenameTagInINI refused: %s -> %s"), *FromTag, *ToTag),
            EMCPErrorCode::Internal,
            TEXT("UE rejected the rename — usually because the destination already exists or the source is in a read-only list. Check the editor log."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("from"), FromTag);
    ResultObj->SetStringField(TEXT("to"), ToTag);
    ResultObj->SetBoolField(TEXT("rename_children"), bRenameChildren);
    ResultObj->SetBoolField(TEXT("redirector_written"), true);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}
