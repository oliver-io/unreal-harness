#include "Commands/MCPFontImportCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Fonts/CompositeFont.h"
#include "Factories/FontFileImportFactory.h"
#include "UObject/Package.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

FMCPFontImportCommands::FMCPFontImportCommands()
{
}

TSharedPtr<FJsonObject> FMCPFontImportCommands::HandleCommand(
	const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("asset_import_font"))
	{
		return HandleImportFont(Params);
	}
	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown font-import command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be one of: asset_import_font."));
}

TSharedPtr<FJsonObject> FMCPFontImportCommands::HandleImportFont(
	const TSharedPtr<FJsonObject>& Params)
{
	FString DestFolder;
	if (!Params->TryGetStringField(TEXT("destination_folder"), DestFolder))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'destination_folder' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`destination_folder` is required and must be a `/Game/...` content-root path (no `.uasset` suffix), e.g. `/Game/UI/Fonts`. The folder is created if it doesn't exist."));
	}
	if (!DestFolder.StartsWith(TEXT("/Game/")))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("destination_folder must start with /Game/ (got %s)"), *DestFolder),
			EMCPErrorCode::InvalidPath,
			TEXT("`destination_folder` must be a `/Game/...` content-root path. Filesystem paths (`C:/...`) and other content roots are not accepted."));
	}
	DestFolder.RemoveFromEnd(TEXT("/"));

	const TArray<TSharedPtr<FJsonValue>>* Fonts = nullptr;
	if (!Params->TryGetArrayField(TEXT("fonts"), Fonts) || Fonts == nullptr || Fonts->Num() == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'fonts' parameter (must be a non-empty array)"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`fonts` is required and must be a non-empty JSON array of objects, each with at least `path` (filesystem path to a source .ttf/.otf) and optionally `name`."));
	}

	bool bForceOverwrite = true;
	Params->TryGetBoolField(TEXT("force_overwrite"), bForceOverwrite);

	FAssetToolsModule& AssetToolsModule =
		FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

	TArray<TSharedPtr<FJsonValue>> Imported;
	TArray<TSharedPtr<FJsonValue>> Failures;

	auto Fail = [&Failures](const FString& Path, const FString& Reason)
	{
		TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
		F->SetStringField(TEXT("path"), Path);
		F->SetStringField(TEXT("reason"), Reason);
		Failures.Add(MakeShared<FJsonValueObject>(F));
	};

	for (const TSharedPtr<FJsonValue>& Entry : *Fonts)
	{
		const TSharedPtr<FJsonObject>* FontObj = nullptr;
		if (!Entry->TryGetObject(FontObj) || FontObj == nullptr) { continue; }
		const TSharedPtr<FJsonObject>& Fnt = *FontObj;

		FString SrcPath;
		if (!Fnt->TryGetStringField(TEXT("path"), SrcPath)) { Fail(TEXT(""), TEXT("missing 'path' field")); continue; }
		const FString FullSrc = FPaths::ConvertRelativePathToFull(SrcPath);
		if (!FPaths::FileExists(FullSrc)) { Fail(FullSrc, TEXT("source file does not exist")); continue; }

		FString AssetName;
		if (!Fnt->TryGetStringField(TEXT("name"), AssetName) || AssetName.IsEmpty())
		{
			AssetName = FPaths::GetBaseFilename(FullSrc);
		}
		const FString FaceName = AssetName + TEXT("_Face");
		const FString FontPkgPath = DestFolder / AssetName;

		if (!bForceOverwrite && UEditorAssetLibrary::DoesAssetExist(FontPkgPath))
		{
			Fail(FullSrc, FString::Printf(TEXT("asset already exists at %s (force_overwrite=false)"), *FontPkgPath));
			continue;
		}

		// 1) Import the raw face (.ttf/.otf → UFontFace) via the engine font factory.
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->AddToRoot();
		Task->Filename         = FullSrc;
		Task->DestinationPath  = DestFolder;
		Task->DestinationName  = FaceName;
		Task->bAutomated       = true;
		Task->bReplaceExisting = bForceOverwrite;
		Task->bSave            = false;
		Task->Factory          = NewObject<UFontFileImportFactory>();
		AssetToolsModule.Get().ImportAssetTasks({ Task });
		Task->RemoveFromRoot();

		if (Task->ImportedObjectPaths.Num() == 0)
		{
			Fail(FullSrc, TEXT("ImportAssetTasks produced no UFontFace (UFontFileImportFactory rejected the file — not a valid .ttf/.otf). Check the editor Output Log (LogFont/LogFactory)."));
			continue;
		}
		UFontFace* FontFace = Cast<UFontFace>(StaticLoadObject(UFontFace::StaticClass(), nullptr, *Task->ImportedObjectPaths[0]));
		if (FontFace == nullptr)
		{
			Fail(FullSrc, FString::Printf(TEXT("imported asset at %s is not a UFontFace"), *Task->ImportedObjectPaths[0]));
			continue;
		}

		// 2) Build a runtime UFont referencing the face (this is what FSlateFontInfo / UMG consume).
		UPackage* Package = CreatePackage(*FontPkgPath);
		if (Package == nullptr) { Fail(FullSrc, FString::Printf(TEXT("CreatePackage failed for %s"), *FontPkgPath)); continue; }

		UFont* Font = NewObject<UFont>(Package, *AssetName, RF_Public | RF_Standalone);
		Font->FontCacheType = EFontCacheType::Runtime;
		FTypefaceEntry TypefaceEntry;
		TypefaceEntry.Name = FName(TEXT("Regular"));
		TypefaceEntry.Font = FFontData(FontFace);
		Font->CompositeFont.DefaultTypeface.Fonts.Add(TypefaceEntry);

		FAssetRegistryModule::AssetCreated(Font);
		Font->MarkPackageDirty();

		const bool bFaceSaved = UEditorAssetLibrary::SaveLoadedAsset(FontFace, /*bOnlyIfIsDirty=*/false);
		const bool bFontSaved = UEditorAssetLibrary::SaveAsset(FontPkgPath, /*bOnlyIfIsDirty=*/false);
		if (!bFaceSaved || !bFontSaved)
		{
			Fail(FullSrc, FString::Printf(TEXT("imported in-memory but save failed (face=%d font=%d) — PIE live or save error; assets will be GC'd"),
				bFaceSaved ? 1 : 0, bFontSaved ? 1 : 0));
			continue;
		}

		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("asset_path"), FontPkgPath + TEXT(".") + AssetName);
		Out->SetStringField(TEXT("face_path"), Task->ImportedObjectPaths[0]);
		Out->SetStringField(TEXT("typeface"), TEXT("Regular"));
		Imported.Add(MakeShared<FJsonValueObject>(Out));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), Imported.Num());
	Data->SetArrayField(TEXT("imported"), Imported);
	Data->SetArrayField(TEXT("failed"), Failures);
	return Data;
}
