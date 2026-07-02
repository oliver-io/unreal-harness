// See AssetManager.h for the full API.

#include "Commands/AssetManager.h"
#include "Commands/MCPCommonUtils.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "Engine/Blueprint.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "RenderingThread.h"
#include "Subsystems/AssetEditorSubsystem.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FAssetManager::CreateSuccessResponse(const TSharedPtr<FJsonObject>& Data)
{
	return FMCPCommonUtils::CreateSuccessResponse(Data);
}

bool FAssetManager::IsValidPackagePath(const FString& Path)
{
	return Path.StartsWith(TEXT("/")) && Path.Len() > 1;
}

// Check asset existence using multiple strategies. LoadAsset is tried first
// (forces load for unscanned marketplace packs), then DoesAssetExist, then
// a direct Asset Registry lookup as a final fallback.
static bool AssetExists(const FString& Path)
{
	if (UEditorAssetLibrary::LoadAsset(Path) != nullptr)
		return true;

	if (UEditorAssetLibrary::DoesAssetExist(Path))
		return true;

	// Final fallback: query the Asset Registry directly with the object path
	FAssetRegistryModule& Module =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& Registry = Module.Get();

	FString ObjectPath = Path;
	if (!ObjectPath.Contains(TEXT(".")))
	{
		ObjectPath = Path + TEXT(".") + FPackageName::GetShortName(Path);
	}
	return Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
}

// Resolve a path to a UObject WITHOUT going through UEditorAssetLibrary::LoadAsset.
// That library path is gated by EditorScriptingHelpers::CheckIfInEditorAndPIE and
// returns null while a PIE session is active, while a freshly-imported asset is
// still async-compiling, or under an -unattended launch — which is the root of the
// intermittent "LoadAsset returned null (corrupt or unloadable package)" on a
// package that demonstrably exists (docs/bugs/mcp.md save_asset entry). We prefer
// the ALREADY-LOADED object (the common save_asset case: the package was just
// mutated in memory by a prior MCP command), then fall back to a raw engine load.
static UObject* ResolveAssetObjectUngated(const FString& InPath)
{
	FString ObjectPath = InPath;
	if (!ObjectPath.Contains(TEXT(".")))
	{
		ObjectPath = InPath + TEXT(".") + FPackageName::GetShortName(InPath);
	}
	if (UObject* Found = FindObject<UObject>(nullptr, *ObjectPath))
	{
		return Found;
	}
	return LoadObject<UObject>(nullptr, *ObjectPath);
}

// ---------------------------------------------------------------------------
// RenameAsset
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FAssetManager::RenameAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath;
	if (!Params->TryGetStringField(TEXT("source_path"), SourcePath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'source_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`source_path` is required and must be the full `/Game/...` asset path to operate on. Use `list_assets` to discover."));
	}

	FString NewName;
	if (!Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'new_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`new_name` is required — the new short asset name (no path, no `.uasset` extension). The rename keeps the asset in the same directory."));
	}

	if (!IsValidPackagePath(SourcePath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Invalid source_path: %s"), *SourcePath),
			EMCPErrorCode::InvalidPath,
			TEXT("`source_path` must be a `/Game/...` package path. Engine paths (`/Engine/...`), relative paths, or bare names are not accepted."));
	}

	if (!AssetExists(SourcePath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Asset not found: %s"), *SourcePath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`source_path` did not resolve to any asset in the Asset Registry. Verify the path exists. Use `list_assets` to discover, or `fixup_redirectors` if the asset was recently moved."));
	}

	// Build destination path: same directory, new name
	FString Directory = FPackageName::GetLongPackagePath(SourcePath);
	FString DestPath = Directory / NewName;

	if (AssetExists(DestPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("An asset already exists at: %s"), *DestPath),
			EMCPErrorCode::NameCollision,
			TEXT("Destination path is already occupied by another asset. Pick a different name/path, or call `delete_asset` on the existing one first."));
	}

	// dry_run: every preflight check above ran already; emit the diff and bail
	// before the actual UEditorAssetLibrary::RenameAsset call. (See todo/13_dry_run_plumbing.md.)
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TSharedPtr<FJsonObject> RenameEntry = MakeShared<FJsonObject>();
		RenameEntry->SetStringField(TEXT("from"), SourcePath);
		RenameEntry->SetStringField(TEXT("to"), DestPath);

		TArray<TSharedPtr<FJsonValue>> RenamedArr;
		RenamedArr.Add(MakeShared<FJsonValueObject>(RenameEntry));

		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("renamed"), RenamedArr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	bool bSuccess = UEditorAssetLibrary::RenameAsset(SourcePath, DestPath);

	if (!bSuccess)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to rename '%s' to '%s'"), *SourcePath, *DestPath),
			EMCPErrorCode::Internal,
			TEXT("`UEditorAssetLibrary::RenameAsset` returned false. Common causes: the asset is in use / has an open editor, the package is checked out by source control, or a transient AssetRegistry inconsistency. Close any open editors for the asset and retry."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("old_path"), SourcePath);
	Data->SetStringField(TEXT("new_path"), DestPath);
	Data->SetStringField(TEXT("new_name"), NewName);

	return CreateSuccessResponse(Data);
}

// ---------------------------------------------------------------------------
// MoveAsset
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FAssetManager::MoveAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath;
	if (!Params->TryGetStringField(TEXT("source_path"), SourcePath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'source_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`source_path` is required and must be the full `/Game/...` asset path to operate on. Use `list_assets` to discover."));
	}

	FString DestFolder;
	if (!Params->TryGetStringField(TEXT("destination_folder"), DestFolder))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'destination_folder' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`destination_folder` is required — the `/Game/...` folder path to move the asset into (without the asset name; the asset keeps its name unless `new_name` is also passed)."));
	}

	if (!IsValidPackagePath(SourcePath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Invalid source_path: %s"), *SourcePath),
			EMCPErrorCode::InvalidPath,
			TEXT("`source_path` must be a `/Game/...` package path. Engine paths (`/Engine/...`), relative paths, or bare names are not accepted."));
	}

	if (!AssetExists(SourcePath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Asset not found: %s"), *SourcePath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`source_path` did not resolve to any asset in the Asset Registry. Verify the path exists. Use `list_assets` to discover, or `fixup_redirectors` if the asset was recently moved."));
	}

	// Determine asset name: use provided new_name or keep original
	FString AssetName;
	if (!Params->TryGetStringField(TEXT("new_name"), AssetName))
	{
		AssetName = FPackageName::GetShortName(SourcePath);
		// Strip the trailing asset name portion after the last dot if present
		int32 DotIndex;
		if (AssetName.FindLastChar('.', DotIndex))
		{
			AssetName = AssetName.Left(DotIndex);
		}
	}

	FString DestPath = DestFolder / AssetName;

	if (AssetExists(DestPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("An asset already exists at: %s"), *DestPath),
			EMCPErrorCode::NameCollision,
			TEXT("Destination path is already occupied by another asset. Pick a different name/path, or call `delete_asset` on the existing one first."));
	}

	// dry_run: emit diff before any side effect (load, rename). Validation
	// already ran above. Diff shape per todo/13: {moved: [{from, to}]}.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TSharedPtr<FJsonObject> MoveEntry = MakeShared<FJsonObject>();
		MoveEntry->SetStringField(TEXT("from"), SourcePath);
		MoveEntry->SetStringField(TEXT("to"),   DestPath);

		TArray<TSharedPtr<FJsonValue>> MovedArr;
		MovedArr.Add(MakeShared<FJsonValueObject>(MoveEntry));

		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("moved"), MovedArr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	// Force-load the asset via Asset Registry before calling RenameAsset.
	// UEditorAssetLibrary::RenameAsset needs the asset loaded, but its own
	// internal load can fail for some asset types. Pre-loading via AssetData
	// ensures the package is in memory.
	FAssetRegistryModule& RegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& Registry = RegistryModule.Get();

	FString ObjectPath = SourcePath;
	if (!ObjectPath.Contains(TEXT(".")))
	{
		ObjectPath = SourcePath + TEXT(".") + FPackageName::GetShortName(SourcePath);
	}
	FAssetData AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	if (AssetData.IsValid())
	{
		AssetData.GetAsset(); // Force load into memory
	}

	// Pre-detect compiled-in (C++ source) referencers. AssetRenameManager
	// detects these and shows a confirmation modal — under the bridge's
	// unattended-script guard the modal returns its default (Cancel), so the
	// rename aborts silently with no useful diagnostic. Surface the specific
	// referencing packages up-front so the caller can rewrite source/INI refs
	// first. See AssetRenameManager.cpp:463-467 for the engine-side check.
	FName SourcePackageName(*FPackageName::ObjectPathToPackageName(ObjectPath));
	TArray<FName> Referencers;
	Registry.GetReferencers(SourcePackageName, Referencers);
	TArray<FString> CompiledInReferencers;
	for (FName Ref : Referencers)
	{
		FNameBuilder RefName(Ref);
		if (UPackage* Pkg = FindPackage(nullptr, *RefName))
		{
			if (Pkg->HasAnyPackageFlags(PKG_CompiledIn))
			{
				CompiledInReferencers.Add(RefName.ToString());
			}
		}
	}
	if (CompiledInReferencers.Num() > 0)
	{
		FString RefList;
		for (const FString& R : CompiledInReferencers)
		{
			RefList += FString::Printf(TEXT("\n  - %s"), *R);
		}
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Cannot move '%s': referenced by %d compiled-in package(s) — text-file rewrite required first"),
				*SourcePath, CompiledInReferencers.Num()),
			EMCPErrorCode::InvalidArgument,
			FString::Printf(TEXT("The following packages contain native (C++) references to this asset (FSoftObjectPath / LoadObject literals in code/INI):%s\n\nFix these text-file references to the new path FIRST, then retry the move. AssetRenameManager refuses to proceed otherwise (its `Source code, config INI, and text files may need Find/Replace` modal defaults to Cancel under the MCP unattended-script guard, which is why the bare rename returns a generic failure). After updating the source: rebuild only the affected module(s), then retry move_asset. See docs/bugs/mcp.md `move_asset — silent failure on assets with text-file references`."),
				*RefList));
	}

	bool bSuccess = UEditorAssetLibrary::RenameAsset(SourcePath, DestPath);

	if (!bSuccess)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to move '%s' to '%s'"), *SourcePath, *DestPath),
			EMCPErrorCode::Internal,
			TEXT("`UEditorAssetLibrary::RenameAsset` returned false after the compiled-in-reference pre-check passed. Remaining causes: (1) the asset is in use / has an open editor — close it. (2) the destination folder doesn't exist or source-control checkout failed. (3) the asset has soft references in CDO defaults that the rename manager still rejects. Check the editor log for `AssetRenameManager` messages."));
	}

	// Drain barrier: FlushRenderingCommands waits for the render thread to
	// finish any pending work (thumbnail refresh, viewport invalidation, etc.)
	// kicked off by the rename. Without this, rapid-fire move_asset batches
	// would queue moves faster than the render thread could drain the
	// per-move work, leading to D3D12 viewport contention and
	// `E_ACCESSDENIED` crashes from WindowsD3D12Viewport.cpp. See
	// docs/bugs/mcp.md `move_asset — D3D12 access-violation crash under
	// rapid-fire batches`.
	FlushRenderingCommands();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("old_path"), SourcePath);
	Data->SetStringField(TEXT("new_path"), DestPath);
	Data->SetStringField(TEXT("destination_folder"), DestFolder);

	return CreateSuccessResponse(Data);
}

// ---------------------------------------------------------------------------
// DuplicateAsset
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FAssetManager::DuplicateAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath;
	if (!Params->TryGetStringField(TEXT("source_path"), SourcePath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'source_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`source_path` is required and must be the full `/Game/...` asset path to operate on. Use `list_assets` to discover."));
	}

	FString DestPath;
	if (!Params->TryGetStringField(TEXT("destination_path"), DestPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'destination_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`destination_path` is required — the full `/Game/...` package path where the duplicate should land (folder + asset name)."));
	}

	if (!IsValidPackagePath(SourcePath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Invalid source_path: %s"), *SourcePath),
			EMCPErrorCode::InvalidPath,
			TEXT("`source_path` must be a `/Game/...` package path. Engine paths (`/Engine/...`), relative paths, or bare names are not accepted."));
	}

	if (!AssetExists(SourcePath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Asset not found: %s"), *SourcePath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`source_path` did not resolve to any asset in the Asset Registry. Verify the path exists. Use `list_assets` to discover, or `fixup_redirectors` if the asset was recently moved."));
	}

	if (AssetExists(DestPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("An asset already exists at: %s"), *DestPath),
			EMCPErrorCode::NameCollision,
			TEXT("Destination path is already occupied by another asset. Pick a different name/path, or call `delete_asset` on the existing one first."));
	}

	// dry_run: emit diff before the actual duplicate. Diff shape per todo/13:
	// {created: [{path, source_path}]}. Source is included so the agent can
	// distinguish a fresh-create from a duplicate-of-X if multiple ops compose.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TSharedPtr<FJsonObject> CreateEntry = MakeShared<FJsonObject>();
		CreateEntry->SetStringField(TEXT("path"),        DestPath);
		CreateEntry->SetStringField(TEXT("source_path"), SourcePath);

		TArray<TSharedPtr<FJsonValue>> CreatedArr;
		CreatedArr.Add(MakeShared<FJsonValueObject>(CreateEntry));

		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("created"), CreatedArr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	UObject* DuplicatedAsset = UEditorAssetLibrary::DuplicateAsset(SourcePath, DestPath);

	if (!DuplicatedAsset)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to duplicate '%s' to '%s'"), *SourcePath, *DestPath),
			EMCPErrorCode::Internal,
			TEXT("`UEditorAssetLibrary::DuplicateAsset` returned null. Common causes: the source asset class doesn't support duplication, the destination folder doesn't exist, or a transient AssetRegistry inconsistency. Verify both paths and retry."));
	}

	// Persist the duplicated asset by default. UEditorAssetLibrary::DuplicateAsset
	// creates the package in-memory only; without a save it is lost on editor restart.
	// Use the file's PromptForCheckoutAndSave idiom (real EPromptReturnCode reason)
	// rather than the bare-bool SaveAsset this file deliberately avoids.
	if (UPackage* DupPackage = DuplicatedAsset->GetPackage())
	{
		TArray<UPackage*> PackagesToSave{DupPackage};
		const FEditorFileUtils::EPromptReturnCode Code = FEditorFileUtils::PromptForCheckoutAndSave(
			PackagesToSave, /*bCheckDirty=*/ false, /*bPromptToSave=*/ false);
		if (Code != FEditorFileUtils::PR_Success)
		{
			FString Reason;
			switch (Code)
			{
				case FEditorFileUtils::PR_Failure:   Reason = TEXT("PromptForCheckoutAndSave failure (check editor log for serializer / SCC errors)"); break;
				case FEditorFileUtils::PR_Cancelled: Reason = TEXT("save cancelled (unattended-script guard auto-cancelled a confirmation modal — likely SCC checkout or Save Content prompt)"); break;
				case FEditorFileUtils::PR_Declined:  Reason = TEXT("save declined (user / unattended path opted out)"); break;
				default:                             Reason = FString::Printf(TEXT("unknown EPromptReturnCode=%d"), static_cast<int32>(Code)); break;
			}
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Duplicated '%s' to '%s' in-memory but failed to persist: %s"), *SourcePath, *DestPath, *Reason),
				EMCPErrorCode::Internal,
				TEXT("The duplicated asset was created in-memory but FEditorFileUtils::PromptForCheckoutAndSave did not write it to disk — it will be lost on editor restart. See the reason string; commonly PIE is active or the destination package is read-only / checked out."));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source_path"), SourcePath);
	Data->SetStringField(TEXT("destination_path"), DestPath);
	Data->SetStringField(TEXT("asset_class"), DuplicatedAsset->GetClass()->GetName());

	return CreateSuccessResponse(Data);
}

// ---------------------------------------------------------------------------
// SaveAsset
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FAssetManager::SaveAsset(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* AssetPathsJson = nullptr;
	TArray<FString> AssetPaths;

	if (Params->TryGetArrayField(TEXT("asset_paths"), AssetPathsJson) && AssetPathsJson)
	{
		for (const TSharedPtr<FJsonValue>& Val : *AssetPathsJson)
		{
			FString Path;
			if (Val->TryGetString(Path) && !Path.IsEmpty())
			{
				AssetPaths.Add(Path);
			}
		}
	}

	TArray<FString> SavedPaths;
	TArray<TPair<FString, FString>> FailedPaths;  // (path, reason)

	if (AssetPaths.Num() == 0)
	{
		// Save all dirty packages
		bool bSuccess = FEditorFileUtils::SaveDirtyPackages(
			/*bPromptUserToSave=*/ false,
			/*bSaveMapPackages=*/ true,
			/*bSaveContentPackages=*/ true);

		if (!bSuccess)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("SaveDirtyPackages reported failure — one or more dirty packages were not written to disk"),
				EMCPErrorCode::Internal,
				TEXT("`FEditorFileUtils::SaveDirtyPackages` returned false: at least one dirty package failed to serialize/save (read-only file, SCC checkout failure, or a serializer error). The edits remain in-memory only and will be lost on editor restart. Check the editor log for per-package save errors."));
		}

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("saved_all_dirty"), bSuccess);
		return CreateSuccessResponse(Data);
	}

	for (const FString& AssetPath : AssetPaths)
	{
		if (!AssetExists(AssetPath))
		{
			FailedPaths.Add({AssetPath, TEXT("asset not found in registry")});
			continue;
		}

		// UEditorAssetLibrary::SaveAsset returns a bare bool with no error info.
		// Route the same FEditorFileUtils::PromptForCheckoutAndSave call directly
		// so we can surface a concrete EPromptReturnCode reason — silent
		// `failed: [path]` was the long-standing UX cliff (docs/bugs/mcp.md
		// `asset_save reports success at envelope while data shows failure`).
		// Resolve via the ungated path (prefers the in-memory object) — NOT
		// UEditorAssetLibrary::LoadAsset, which returns null during PIE / async
		// compile / -unattended and produced the false "corrupt or unloadable"
		// failures (docs/bugs/mcp.md).
		UObject* AssetObj = ResolveAssetObjectUngated(AssetPath);
		if (!AssetObj)
		{
			FailedPaths.Add({AssetPath, TEXT("could not resolve object (not loaded and raw LoadObject failed)")});
			continue;
		}
		UPackage* Package = AssetObj->GetPackage();
		if (!Package)
		{
			FailedPaths.Add({AssetPath, TEXT("asset has no outer package")});
			continue;
		}

		TArray<UPackage*> PackagesToSave{Package};
		const FEditorFileUtils::EPromptReturnCode Code = FEditorFileUtils::PromptForCheckoutAndSave(
			PackagesToSave, /*bCheckDirty=*/ false, /*bPromptToSave=*/ false);

		if (Code == FEditorFileUtils::PR_Success)
		{
			SavedPaths.Add(AssetPath);
		}
		else
		{
			FString Reason;
			switch (Code)
			{
				case FEditorFileUtils::PR_Failure:   Reason = TEXT("PromptForCheckoutAndSave failure (check editor log for serializer / SCC errors)"); break;
				case FEditorFileUtils::PR_Cancelled: Reason = TEXT("save cancelled (unattended-script guard auto-cancelled a confirmation modal — likely SCC checkout or Save Content prompt)"); break;
				case FEditorFileUtils::PR_Declined:  Reason = TEXT("save declined (user / unattended path opted out)"); break;
				default:                             Reason = FString::Printf(TEXT("unknown EPromptReturnCode=%d"), static_cast<int32>(Code)); break;
			}
			FailedPaths.Add({AssetPath, Reason});
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> SavedArray;
	for (const FString& P : SavedPaths)
	{
		SavedArray.Add(MakeShared<FJsonValueString>(P));
	}
	Data->SetArrayField(TEXT("saved"), SavedArray);

	TArray<TSharedPtr<FJsonValue>> FailedArray;
	for (const TPair<FString, FString>& P : FailedPaths)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"),   P.Key);
		Entry->SetStringField(TEXT("reason"), P.Value);
		FailedArray.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Data->SetArrayField(TEXT("failed"), FailedArray);

	return CreateSuccessResponse(Data);
}

// ---------------------------------------------------------------------------
// ListAssets
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FAssetManager::ListAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString DirectoryPath;
	if (!Params->TryGetStringField(TEXT("directory_path"), DirectoryPath))
	{
		DirectoryPath = TEXT("/Game/");
	}

	bool bRecursive = true;
	Params->TryGetBoolField(TEXT("recursive"), bRecursive);

	FString ClassFilter;
	Params->TryGetStringField(TEXT("class_filter"), ClassFilter);

	// Use the Asset Registry directly — UEditorAssetLibrary::ListAssets misses
	// assets not yet scanned on fresh editor startup.
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	if (DirectoryPath.Len() >= NAME_SIZE)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("directory_path too long (%d chars; max %d)"), DirectoryPath.Len(), NAME_SIZE - 1),
			EMCPErrorCode::InvalidPath,
			TEXT("`directory_path` exceeds the engine's FName length limit. Converting a string this long to an FName for the Asset Registry filter trips the engine's FName length assertion (checkf in FindOrStoreString) and fatally crashes the editor. Pass a normal `/Game/...` path."));
	}

	if (DirectoryPath.Len() >= NAME_SIZE)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("directory_path too long (%d chars; max %d)"), DirectoryPath.Len(), NAME_SIZE - 1),
			EMCPErrorCode::InvalidPath,
			TEXT("`directory_path` exceeds the engine's FName length limit. Converting a string this long to an FName for the Asset Registry filter trips the engine's FName length assertion (checkf in FindOrStoreString) and fatally crashes the editor. Pass a normal `/Game/...` path."));
	}

	FARFilter Filter;
	Filter.PackagePaths.Add(*DirectoryPath);
	Filter.bRecursivePaths = bRecursive;

	TArray<FAssetData> AllAssets;
	AssetRegistry.GetAssets(Filter, AllAssets);

	TArray<TSharedPtr<FJsonValue>> AssetArray;

	for (const FAssetData& AssetData : AllAssets)
	{
		FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();

		if (!ClassFilter.IsEmpty() && !ClassName.Contains(ClassFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
		Entry->SetStringField(TEXT("class"), ClassName);
		Entry->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		AssetArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), AssetArray.Num());
	Data->SetArrayField(TEXT("assets"), AssetArray);

	return CreateSuccessResponse(Data);
}

// ---------------------------------------------------------------------------
// FixupRedirectors
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FAssetManager::FixupRedirectors(const TSharedPtr<FJsonObject>& Params)
{
	FString DirectoryPath;
	if (!Params->TryGetStringField(TEXT("directory_path"), DirectoryPath))
	{
		DirectoryPath = TEXT("/Game/");
	}

	// Find all redirectors under the given path
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	if (DirectoryPath.Len() >= NAME_SIZE)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("directory_path too long (%d chars; max %d)"), DirectoryPath.Len(), NAME_SIZE - 1),
			EMCPErrorCode::InvalidPath,
			TEXT("`directory_path` exceeds the engine's FName length limit. Converting a string this long to an FName for the Asset Registry filter trips the engine's FName length assertion (checkf in FindOrStoreString) and fatally crashes the editor. Pass a normal `/Game/...` path."));
	}

	if (DirectoryPath.Len() >= NAME_SIZE)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("directory_path too long (%d chars; max %d)"), DirectoryPath.Len(), NAME_SIZE - 1),
			EMCPErrorCode::InvalidPath,
			TEXT("`directory_path` exceeds the engine's FName length limit. Converting a string this long to an FName for the Asset Registry filter trips the engine's FName length assertion (checkf in FindOrStoreString) and fatally crashes the editor. Pass a normal `/Game/...` path."));
	}

	FARFilter Filter;
	Filter.PackagePaths.Add(*DirectoryPath);
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());

	TArray<FAssetData> RedirectorAssets;
	AssetRegistry.GetAssets(Filter, RedirectorAssets);

	if (RedirectorAssets.Num() == 0)
	{
		// Empty case is the same shape on both paths — fixup is a no-op when
		// no redirectors exist. Surface the dry_run flag in the success
		// response so callers can branch consistently.
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("redirectors_found"), 0);
		Data->SetStringField(TEXT("message"), TEXT("No redirectors found under the given path"));
		if (FMCPCommonUtils::ParseDryRun(Params))
		{
			Data->SetBoolField(TEXT("dry_run"), true);
		}
		return CreateSuccessResponse(Data);
	}

	// dry_run: emit diff listing every redirector that *would* be removed.
	// Validation already ran (path exists, registry was queried). The actual
	// fixup operation rewires references in pointing assets and then deletes
	// the redirector packages — both effects are captured by the deleted[]
	// shape, since redirectors are the visible artifacts that disappear.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TArray<TSharedPtr<FJsonValue>> DeletedArr;
		DeletedArr.Reserve(RedirectorAssets.Num());
		for (const FAssetData& AssetData : RedirectorAssets)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
			DeletedArr.Add(MakeShared<FJsonValueObject>(Entry));
		}
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("deleted"), DeletedArr);
		TSharedPtr<FJsonObject> Wrapped = FMCPCommonUtils::CreateDryRunResponse(Diff);
		Wrapped->SetNumberField(TEXT("redirectors_found"), RedirectorAssets.Num());
		return Wrapped;
	}

	// Load the redirector objects
	TArray<UObjectRedirector*> Redirectors;
	for (const FAssetData& AssetData : RedirectorAssets)
	{
		UObject* Obj = AssetData.GetAsset();
		UObjectRedirector* Redirector = Cast<UObjectRedirector>(Obj);
		if (Redirector)
		{
			Redirectors.Add(Redirector);
		}
	}

	// Headless port of FAssetFixUpRedirectors::ExecuteFixUp
	// (Engine/Source/Developer/AssetTools/Private/AssetFixUpRedirectors.cpp:562).
	//
	// We can't call IAssetTools::FixupReferencers — its tail end is
	// `SFixupRedirectorsReport::ShowModalDialog(...)` at line 939, which under
	// unattended mode returns an unset TOptional and the next `GetValue()` asserts,
	// crashing the editor. The modal only confirms the delete step; the actual
	// reference-rewriting work has already happened by that point. Skip the modal
	// and execute the same pipeline through public APIs.
	//
	// Pipeline (matches the engine version step-for-step):
	//   1. Gather referencers for each redirector via AssetRegistry
	//   2. Load each referencing package (recording temp-loads for cleanup)
	//   3. Root referencing-package objects so the rewrite doesn't GC them mid-flight
	//   4. Build old→new FSoftObjectPath map (+ Blueprint Class / DefaultObject variants)
	//   5. IAssetTools::RenameReferencingSoftObjectPaths — the actual fixup
	//   6. PromptForCheckoutAndSave the rewritten packages (no prompt)
	//   7. Delete redirectors whose referencers all saved cleanly
	//      (leave a redirector intact if any of its referencers failed to save —
	//      removing it would orphan those references)
	//   8. UnloadPackages for anything we temp-loaded
	//
	// Intentionally skipped relative to the engine version:
	//   - SCC checkout step (PromptForCheckoutAndSave handles it internally in
	//     unattended mode)
	//   - CollectionManager::HandleRedirectorDeleted (niche — leaves stale collection
	//     entries but does not corrupt asset references; would need CollectionManager
	//     in Build.cs as a new module dep)
	//   - LevelInstance loader reset (niche)
	//   - Asset Registry path rescan (perf-only, no correctness impact)
	//   - SFixupRedirectorsReport modal (the crash site)

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

	// 1. Gather referencers
	struct FRedirectorEntry
	{
		UObjectRedirector* Redirector;
		FName              PackageName;
		TArray<FName>      ReferencingPackageNames;
	};
	TArray<FRedirectorEntry> Entries;
	Entries.Reserve(Redirectors.Num());
	for (UObjectRedirector* R : Redirectors)
	{
		FRedirectorEntry E;
		E.Redirector  = R;
		E.PackageName = R->GetOutermost()->GetFName();
		AssetRegistry.GetReferencers(E.PackageName, E.ReferencingPackageNames);
		Entries.Add(MoveTemp(E));
	}

	// 2. Load referencing packages (skip script/compiled-in references — those are
	// .cpp `LoadObject<>` / `FSoftObjectPath` literals and can't be rewritten here;
	// the move_asset text-ref entry covers that workflow)
	TSet<UPackage*> PackagesToSave;
	TSet<UPackage*> TempLoadedPackages;
	TSet<FName>     CompiledInReferencers;  // tracked for the response payload
	for (const FRedirectorEntry& E : Entries)
	{
		for (FName RefName : E.ReferencingPackageNames)
		{
			FNameBuilder PackageName(RefName);
			UPackage* Pkg = FindPackage(nullptr, *PackageName);
			if (!Pkg)
			{
				Pkg = LoadPackage(nullptr, *PackageName, LOAD_None);
				if (Pkg)
				{
					TempLoadedPackages.Add(Pkg);
				}
			}
			if (Pkg)
			{
				if (Pkg->HasAnyPackageFlags(PKG_CompiledIn))
				{
					CompiledInReferencers.Add(RefName);
				}
				else
				{
					PackagesToSave.Add(Pkg);
				}
			}
		}
	}

	// 3. Root referencing-package objects so they survive GC during the rewrite.
	// Matches FAssetFixUpRedirectors lines 732-741.
	TArray<TStrongObjectPtr<UObject>> RootedObjects;
	for (UPackage* Pkg : PackagesToSave)
	{
		ForEachObjectWithPackage(Pkg, [&RootedObjects](UObject* Object)
		{
			RootedObjects.Emplace(Object);
			return true;
		}, /*bIncludeNestedObjects=*/ false, /*ExclusionFlags=*/ RF_Standalone, EInternalObjectFlags::RootSet);
	}

	// 4. Build old→new redirect map. For Blueprint redirector targets, add
	// Class (`_C`) and CDO (`Default__X_C`) variants so generated-class refs
	// are also rewritten (engine path lines 846-861).
	TMap<FSoftObjectPath, FSoftObjectPath> RedirectorMap;
	for (UObjectRedirector* R : Redirectors)
	{
		if (!R || !R->DestinationObject)
		{
			continue;
		}
		FSoftObjectPath OldPath(R);
		FSoftObjectPath NewPath(R->DestinationObject);
		RedirectorMap.Add(OldPath, NewPath);
		if (Cast<UBlueprint>(R->DestinationObject))
		{
			RedirectorMap.Add(
				FSoftObjectPath(FString::Printf(TEXT("%s_C"), *OldPath.ToString())),
				FSoftObjectPath(FString::Printf(TEXT("%s_C"), *NewPath.ToString())));
			RedirectorMap.Add(
				FSoftObjectPath(FString::Printf(TEXT("%s.Default__%s_C"),
					*OldPath.GetLongPackageName(), *OldPath.GetAssetName())),
				FSoftObjectPath(FString::Printf(TEXT("%s.Default__%s_C"),
					*NewPath.GetLongPackageName(), *NewPath.GetAssetName())));
		}
	}

	// 5. Rewrite soft-object references across all loaded referencing packages.
	// This is the actual "fixup" work; everything before this gathers state and
	// everything after persists / cleans up.
	AssetTools.RenameReferencingSoftObjectPaths(PackagesToSave.Array(), RedirectorMap);

	// 6. Save rewritten packages. PromptForCheckoutAndSave checks out via SCC
	// and writes to disk; with bPromptToSave=false and the bridge-wide unattended
	// guard, any internal modals auto-default and the call returns synchronously.
	TArray<UPackage*> FailedToSave;
	if (PackagesToSave.Num() > 0)
	{
		FEditorFileUtils::PromptForCheckoutAndSave(
			PackagesToSave.Array(),
			/*bCheckDirty=*/   false,
			/*bPromptToSave=*/ false,
			&FailedToSave);
	}

	// 7. Delete redirectors whose referencers all saved cleanly. If any
	// referencer failed to save, the redirector stays in place as a bridge
	// (deleting it would orphan the now-stale on-disk reference).
	// Also skip if any code referencer hit it — code can't be auto-rewritten.
	TSet<FName> FailedReferencers;
	for (UPackage* P : FailedToSave)
	{
		FailedReferencers.Add(P->GetFName());
	}

	TArray<UObject*> ObjectsToDelete;
	int32 RedirectorsKept = 0;
	for (const FRedirectorEntry& E : Entries)
	{
		bool bAllReferencersSaved = true;
		for (FName Ref : E.ReferencingPackageNames)
		{
			if (FailedReferencers.Contains(Ref) || CompiledInReferencers.Contains(Ref))
			{
				bAllReferencersSaved = false;
				break;
			}
		}
		if (!bAllReferencersSaved)
		{
			++RedirectorsKept;
			continue;
		}

		UPackage* RedirectorPackage = E.Redirector->GetOutermost();
		bool bContainsAtLeastOneOtherAsset = false;
		ForEachObjectWithOuter(RedirectorPackage, [&ObjectsToDelete, &bContainsAtLeastOneOtherAsset](UObject* Obj)
		{
			if (UObjectRedirector* RR = Cast<UObjectRedirector>(Obj))
			{
				RR->RemoveFromRoot();
				ObjectsToDelete.Add(RR);
			}
			else
			{
				bContainsAtLeastOneOtherAsset = true;
			}
		});
		if (!bContainsAtLeastOneOtherAsset)
		{
			RedirectorPackage->RemoveFromRoot();
			ObjectsToDelete.Add(RedirectorPackage);
		}
	}

	// Release strong-pointer roots before delete so referencers can GC normally.
	RootedObjects.Empty();

	const int32 DeletedCount = (ObjectsToDelete.Num() > 0)
		? ObjectTools::DeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/ false)
		: 0;

	// 8. Unload anything we temp-loaded for the rewrite. Errors here are
	// non-fatal — the rewrite + save already committed.
	if (TempLoadedPackages.Num() > 0)
	{
		FText UnloadError;
		UPackageTools::UnloadPackages(TempLoadedPackages.Array(), UnloadError, /*bUnloadDirtyPackages=*/ true);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("redirectors_found"),         RedirectorAssets.Num());
	Data->SetNumberField(TEXT("redirectors_processed"),     Redirectors.Num());
	Data->SetNumberField(TEXT("redirectors_deleted"),       DeletedCount);
	Data->SetNumberField(TEXT("redirectors_kept"),          RedirectorsKept);
	Data->SetNumberField(TEXT("packages_rewritten"),        PackagesToSave.Num());
	Data->SetNumberField(TEXT("packages_failed_to_save"),   FailedToSave.Num());
	Data->SetNumberField(TEXT("compiled_in_referencers"),   CompiledInReferencers.Num());
	Data->SetStringField(TEXT("directory"),                 DirectoryPath);

	return CreateSuccessResponse(Data);
}

// ---------------------------------------------------------------------------
// DeleteAsset
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FAssetManager::DeleteAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'asset_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`asset_path` is required and must be the full `/Game/...` asset path to operate on. Use `list_assets` to discover."));
	}

	if (!IsValidPackagePath(AssetPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Invalid asset_path: %s"), *AssetPath),
			EMCPErrorCode::InvalidPath,
			TEXT("`asset_path` must be a `/Game/...` package path. Engine paths (`/Engine/...`), relative paths, or bare names are not accepted."));
	}

	if (!AssetExists(AssetPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Asset not found: %s"), *AssetPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`asset_path` did not resolve to any asset in the Asset Registry. Verify the path exists. Use `list_assets` to discover, or `fixup_redirectors` if the asset was recently moved."));
	}

	bool bForce = false;
	Params->TryGetBoolField(TEXT("force"), bForce);

	// Check for referencers unless force is set
	if (!bForce)
	{
		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		FString PackageName = AssetPath;
		int32 DotIndex;
		if (PackageName.FindChar('.', DotIndex))
		{
			PackageName = PackageName.Left(DotIndex);
		}

		TArray<FAssetIdentifier> ReferenceNames;
		AssetRegistry.GetReferencers(FAssetIdentifier(FName(*PackageName)), ReferenceNames);

		if (ReferenceNames.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> RefArray;
			for (const FAssetIdentifier& Ref : ReferenceNames)
			{
				RefArray.Add(MakeShared<FJsonValueString>(Ref.PackageName.ToString()));
			}

			TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
			ErrData->SetBoolField(TEXT("success"), false);
			ErrData->SetStringField(TEXT("error"),
				FString::Printf(TEXT("Asset '%s' is referenced by %d other asset(s). Use force=true to delete anyway."),
					*AssetPath, ReferenceNames.Num()));
			ErrData->SetArrayField(TEXT("referencers"), RefArray);
			return ErrData;
		}
	}

	// dry_run: validation (existence + reference check) is already done above;
	// emit the diff before the actual UEditorAssetLibrary::DeleteAsset call.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TSharedPtr<FJsonObject> DeleteEntry = MakeShared<FJsonObject>();
		DeleteEntry->SetStringField(TEXT("path"), AssetPath);

		TArray<TSharedPtr<FJsonValue>> DeletedArr;
		DeletedArr.Add(MakeShared<FJsonValueObject>(DeleteEntry));

		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("deleted"), DeletedArr);
		// Include the referencer list in the diff if force=true would be needed
		// to commit. The agent reasons on this before deciding to proceed.
		if (bForce)
		{
			FAssetRegistryModule& AssetRegistryModule =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
			FString PackageName = AssetPath;
			int32 DotIndex;
			if (PackageName.FindChar('.', DotIndex))
			{
				PackageName = PackageName.Left(DotIndex);
			}
			TArray<FAssetIdentifier> ReferenceNames;
			AssetRegistry.GetReferencers(FAssetIdentifier(FName(*PackageName)), ReferenceNames);
			TArray<TSharedPtr<FJsonValue>> RefArr;
			for (const FAssetIdentifier& Ref : ReferenceNames)
			{
				RefArr.Add(MakeShared<FJsonValueString>(Ref.PackageName.ToString()));
			}
			Diff->SetArrayField(TEXT("referencers_orphaned"), RefArr);
		}
		TSharedPtr<FJsonObject> Wrapped = FMCPCommonUtils::CreateDryRunResponse(Diff);
		Wrapped->SetBoolField(TEXT("forced"), bForce);
		return Wrapped;
	}

	bool bDeleted = UEditorAssetLibrary::DeleteAsset(AssetPath);

	if (!bDeleted)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to delete asset: %s"), *AssetPath),
			EMCPErrorCode::Internal,
			TEXT("`UEditorAssetLibrary::DeleteAsset` returned false. Common causes: the asset is referenced (pass `force=true` to delete anyway, with referencer-orphaning), the asset has an open editor, or source-control checkout failed. Close open editors and retry."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("deleted_path"), AssetPath);
	Data->SetBoolField(TEXT("forced"), bForce);

	return CreateSuccessResponse(Data);
}

// ---------------------------------------------------------------------------
// OpenAsset
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FAssetManager::OpenAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'asset_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`asset_path` is required and must be the full `/Game/...` asset path to operate on. Use `list_assets` to discover."));
	}

	if (!IsValidPackagePath(AssetPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Invalid asset_path: %s"), *AssetPath),
			EMCPErrorCode::InvalidPath,
			TEXT("`asset_path` must be a `/Game/...` package path. Engine paths (`/Engine/...`), relative paths, or bare names are not accepted."));
	}

	// Boot guard: open_asset during UEditorEngine::Init deadlocks the game thread
	// against an in-flight render-state setup (GameUserSettings::ApplySettings
	// triggers FlushRenderingCommands). The bridge subsystem binds 55557 before
	// the editor finishes booting, so requests can land in this window. Refuse
	// rather than crash — agents can retry once GIsRunning is true.
	if (!GIsRunning)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Editor is still booting — open_asset is unsafe until init completes"),
			EMCPErrorCode::Internal,
			TEXT("Hitting OpenAssetEditor during UEditorEngine::Init causes a game-thread/render-thread deadlock (FlushRenderingCommands fence never returns). Wait for editor boot to finish (typically <30s after launch) and retry. See docs/bugs/mcp.md `Queued OpenAsset MCP command crashes editor during startup`."));
	}

	// Ungated resolve (prefers the in-memory object, falls back to a raw load) —
	// avoids the UEditorAssetLibrary::LoadAsset null it returns during PIE / async
	// compile. PIE itself is already refused at the bridge gate (pie_active).
	UObject* Asset = ResolveAssetObjectUngated(AssetPath);
	if (!Asset)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Asset not found: %s"), *AssetPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`asset_path` did not resolve to any asset. Verify the path exists. Use `list_assets` to discover, or `fixup_redirectors` if the asset was recently moved. If a PIE session is running, stop it first."));
	}

	// Refuse UWorld (.umap) assets. `OpenEditorForAsset` on a level routes into
	// FAssetManager::OpenAsset, which assumes a non-world asset editor and
	// null-derefs on a UWorld → hard EXCEPTION_ACCESS_VIOLATION (no ensure).
	// Changing the loaded editor map needs FEditorFileUtils::LoadMap on the game
	// thread, but doing that synchronously from a bridge command (which runs
	// inside UWorld::Tick) destroys the live world mid-tick — the same crash
	// class as the now-fixed pie_start bug. So refuse with a structured error
	// rather than crash; the caller relaunches the editor (boots into
	// EditorStartupMap) or opens the level by hand. See docs/bugs/mcp.md.
	if (UClass* WorldClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.World")))
	{
		if (Asset->IsA(WorldClass))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Refusing to open a level (UWorld) asset via open_asset: %s"), *AssetPath),
				EMCPErrorCode::UnsupportedClass,
				TEXT("`open_asset` opens an asset editor, which crashes on a level (.umap / UWorld) — OpenEditorForAsset has no world-asset path and null-derefs. To change the loaded editor map, relaunch the editor (it boots into EditorStartupMap) or open the level by hand. Loading a map synchronously from a bridge command would destroy the live world mid-tick (the pie_start crash class), so it is not done here."));
		}
	}

	UAssetEditorSubsystem* AssetEditorSubsystem =
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("AssetEditorSubsystem not available"),
			EMCPErrorCode::Internal,
			TEXT("`GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()` returned null. The editor is mid-initialization or shutting down — retry after the editor has fully loaded."));
	}

	bool bOpened = AssetEditorSubsystem->OpenEditorForAsset(Asset);
	if (!bOpened)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to open editor for asset: %s"), *AssetPath),
			EMCPErrorCode::Internal,
			TEXT("`UAssetEditorSubsystem::OpenEditorForAsset` returned false. Most asset classes have an editor, but some non-editable types (e.g. transient cooked assets) do not. Verify the asset class supports editing."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());

	return CreateSuccessResponse(Data);
}
