#include "Commands/MCPLevelCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "FileHelpers.h" // UEditorLoadingAndSavingUtils, FEditorFileUtils (UnrealEd)
#include "Editor.h"      // GEditor
#include "Editor/EditorEngine.h" // UEditorEngine::BuildReflectionCaptures
#include "Engine/World.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace
{
	// Resolve the active editor world (the one File > Save would persist).
	UWorld* LevelCmd_GetEditorWorld()
	{
		if (GEditor == nullptr)
		{
			return nullptr;
		}
		return GEditor->GetEditorWorldContext().World();
	}

	// Normalize a caller-supplied content path into a bare long package name:
	// strips any object suffix ("/Game/M.M" → "/Game/M") and a trailing slash.
	FString NormalizePackagePath(const FString& InPath)
	{
		FString Path = InPath;
		Path.TrimStartAndEndInline();
		Path.RemoveFromEnd(TEXT("/"));
		// "/Game/Foo.Foo" → "/Game/Foo" (no-op for package-only paths).
		return FPackageName::ObjectPathToPackageName(Path);
	}
}

FMCPLevelCommands::FMCPLevelCommands()
{
}

TSharedPtr<FJsonObject> FMCPLevelCommands::HandleCommand(
	const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("level_new"))
	{
		return HandleNewLevel(Params);
	}
	if (CommandType == TEXT("level_save"))
	{
		return HandleSaveLevel(Params);
	}
	if (CommandType == TEXT("level_save_as"))
	{
		return HandleSaveLevelAs(Params);
	}
	if (CommandType == TEXT("level_load"))
	{
		return HandleLoadLevel(Params);
	}
	if (CommandType == TEXT("editor_build_reflection_captures"))
	{
		return HandleBuildReflectionCaptures(Params);
	}
	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown level command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be one of: level_new, level_save, level_save_as, level_load, editor_build_reflection_captures."));
}

TSharedPtr<FJsonObject> FMCPLevelCommands::HandleNewLevel(
	const TSharedPtr<FJsonObject>& Params)
{
	FString Template;
	Params->TryGetStringField(TEXT("template"), Template);
	Template.TrimStartAndEndInline();

	// Do NOT save the outgoing world — level_new is explicitly "start fresh".
	// Callers that want to keep the current world must level_save first.
	const bool bSaveExisting = false;

	UWorld* NewWorld = nullptr;
	if (Template.IsEmpty())
	{
		NewWorld = UEditorLoadingAndSavingUtils::NewBlankMap(bSaveExisting);
	}
	else
	{
		const FString TemplatePackage = NormalizePackagePath(Template);
		if (!FPackageName::DoesPackageExist(TemplatePackage))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Template level does not exist: %s"), *TemplatePackage),
				EMCPErrorCode::AssetNotFound,
				TEXT("`template` must be a `/Game/...` path to an existing level asset, or omit it (empty) for a blank map."));
		}
		NewWorld = UEditorLoadingAndSavingUtils::NewMapFromTemplate(TemplatePackage, bSaveExisting);
	}

	if (NewWorld == nullptr)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create a new level"),
			EMCPErrorCode::Internal,
			TEXT("UEditorLoadingAndSavingUtils returned no world. Check the editor Output Log; ensure no modal save prompt is blocking and that PIE is stopped."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("created"), true);
	Data->SetStringField(TEXT("map_name"), NewWorld->GetName());
	Data->SetStringField(TEXT("package_path"), NewWorld->GetPackage()->GetName());
	return Data;
}

TSharedPtr<FJsonObject> FMCPLevelCommands::HandleSaveLevel(
	const TSharedPtr<FJsonObject>& /*Params*/)
{
	UWorld* World = LevelCmd_GetEditorWorld();
	if (World == nullptr)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No active editor world to save"),
			EMCPErrorCode::Internal,
			TEXT("GEditor->GetEditorWorldContext().World() returned null. The editor may not be fully initialized."));
	}

	const FString PackageName = World->GetPackage()->GetName();

	// Untitled worlds live under /Temp/ and have no on-disk package — saving them
	// requires a destination path, which is level_save_as's job.
	if (FPackageName::IsTempPackage(PackageName) || !FPackageName::DoesPackageExist(PackageName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Current level has no on-disk package (%s)"), *PackageName),
			EMCPErrorCode::InvalidPath,
			TEXT("This is a transient/untitled level. Use `level_save_as` with a `package_path` (e.g. `/Game/Maps/L_Arena`) to give it a home on disk first."));
	}

	if (!UEditorLoadingAndSavingUtils::SaveMap(World, PackageName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to save level: %s"), *PackageName),
			EMCPErrorCode::Internal,
			TEXT("UEditorLoadingAndSavingUtils::SaveMap returned false. Check the editor Output Log; the package may be read-only or checked out under source control."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("saved"), true);
	Data->SetStringField(TEXT("map_name"), World->GetName());
	Data->SetStringField(TEXT("package_path"), PackageName);
	return Data;
}

TSharedPtr<FJsonObject> FMCPLevelCommands::HandleSaveLevelAs(
	const TSharedPtr<FJsonObject>& Params)
{
	FString RawPath;
	if (!Params->TryGetStringField(TEXT("package_path"), RawPath) || RawPath.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'package_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`package_path` is required and must be a `/Game/...` content path (no extension), e.g. `/Game/Maps/L_Arena`."));
	}
	const FString PackagePath = NormalizePackagePath(RawPath);
	if (!PackagePath.StartsWith(TEXT("/Game/")))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("package_path must start with /Game/ (got %s)"), *PackagePath),
			EMCPErrorCode::InvalidPath,
			TEXT("`package_path` must be a `/Game/...` content-root path. Filesystem paths (`C:/...`) and other content roots are not accepted."));
	}

	UWorld* World = LevelCmd_GetEditorWorld();
	if (World == nullptr)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No active editor world to save"),
			EMCPErrorCode::Internal,
			TEXT("GEditor->GetEditorWorldContext().World() returned null. The editor may not be fully initialized."));
	}

	if (!UEditorLoadingAndSavingUtils::SaveMap(World, PackagePath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to save level to: %s"), *PackagePath),
			EMCPErrorCode::Internal,
			TEXT("UEditorLoadingAndSavingUtils::SaveMap returned false. Verify the package path is valid and writable, and that the parent /Game/ folder exists."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("saved"), true);
	Data->SetStringField(TEXT("map_name"), FPackageName::GetShortName(PackagePath));
	Data->SetStringField(TEXT("package_path"), PackagePath);
	return Data;
}

TSharedPtr<FJsonObject> FMCPLevelCommands::HandleLoadLevel(
	const TSharedPtr<FJsonObject>& Params)
{
	FString RawPath;
	if (!Params->TryGetStringField(TEXT("package_path"), RawPath) || RawPath.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'package_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`package_path` is required and must be a `/Game/...` path to an existing level, e.g. `/Game/Maps/L_Arena`."));
	}
	const FString PackagePath = NormalizePackagePath(RawPath);
	if (!PackagePath.StartsWith(TEXT("/Game/")))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("package_path must start with /Game/ (got %s)"), *PackagePath),
			EMCPErrorCode::InvalidPath,
			TEXT("`package_path` must be a `/Game/...` content-root path."));
	}

	// Pre-validate existence so a missing path returns a clean asset_not_found
	// instead of triggering a modal "bad filename" dialog inside LoadMap (GUI mode).
	if (!FPackageName::DoesPackageExist(PackagePath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Level does not exist: %s"), *PackagePath),
			EMCPErrorCode::AssetNotFound,
			TEXT("No level package found at `package_path`. Use `asset_list` (asset_type='World') to discover levels; paths are case-sensitive and must include the `/Game/` prefix."));
	}

	UWorld* Loaded = UEditorLoadingAndSavingUtils::LoadMap(PackagePath);
	if (Loaded == nullptr)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to load level: %s"), *PackagePath),
			EMCPErrorCode::Internal,
			TEXT("UEditorLoadingAndSavingUtils::LoadMap returned null. Check the editor Output Log; the load may have been aborted (e.g. an active PIE session or an unsaved-world prompt)."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("loaded"), true);
	Data->SetStringField(TEXT("map_name"), Loaded->GetName());
	Data->SetStringField(TEXT("package_path"), Loaded->GetPackage()->GetName());
	return Data;
}

TSharedPtr<FJsonObject> FMCPLevelCommands::HandleBuildReflectionCaptures(
	const TSharedPtr<FJsonObject>& Params)
{
	if (GEditor == nullptr)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("GEditor is null"),
			EMCPErrorCode::Internal,
			TEXT("The editor engine is not available; this command requires a running editor."));
	}

	UWorld* World = LevelCmd_GetEditorWorld();
	if (World == nullptr)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No active editor world"),
			EMCPErrorCode::Internal,
			TEXT("GEditor->GetEditorWorldContext().World() returned null. The editor may not be fully initialized."));
	}

	// Bake the scene into every reflection capture's cubemap and write it into the level's
	// MapBuildDataRegistry — the same path as Build > Build Reflection Captures. This is what
	// clears the stale serialized data that makes PIE/cooked builds look washed-out while the
	// editor viewport (which renders a transient live re-capture) looks fine.
	GEditor->BuildReflectionCaptures(World);

	bool bSave = true;
	Params->TryGetBoolField(TEXT("save"), bSave);

	bool bSaved = false;
	if (bSave)
	{
		// BuildReflectionCaptures dirties the MapBuildDataRegistry (a sidecar package). Persist
		// it (+ the map) so PIE / cooked builds read the freshly-baked cubemaps from disk.
		bSaved = FEditorFileUtils::SaveDirtyPackages(
			/*bPromptUserToSave=*/false,
			/*bSaveMapPackages=*/true,
			/*bSaveContentPackages=*/true);
	}

	const FString PackageName = World->GetPackage()->GetName();
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("built"), true);
	Data->SetStringField(TEXT("map_name"), World->GetName());
	Data->SetStringField(TEXT("package_path"), PackageName);
	Data->SetBoolField(TEXT("saved"), bSaved);
	return Data;
}
