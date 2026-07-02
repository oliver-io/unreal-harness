#include "Commands/MCPSoundImportCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "EditorAssetLibrary.h"
#include "Sound/SoundWave.h"
#include "SoundWaveCompiler.h"
#include "Factories/SoundFactory.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

FMCPSoundImportCommands::FMCPSoundImportCommands()
{
}

TSharedPtr<FJsonObject> FMCPSoundImportCommands::HandleCommand(
	const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("asset_import_audio"))
	{
		return HandleImportAudio(Params);
	}
	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown sound-import command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be one of: asset_import_audio."));
}

TSharedPtr<FJsonObject> FMCPSoundImportCommands::HandleImportAudio(
	const TSharedPtr<FJsonObject>& Params)
{
	FString DestFolder;
	if (!Params->TryGetStringField(TEXT("destination_folder"), DestFolder))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'destination_folder' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`destination_folder` is required and must be a `/Game/...` content-root path (no `.uasset` suffix), e.g. `/Game/Audio/Imported`. The folder is created if it doesn't exist."));
	}
	if (!DestFolder.StartsWith(TEXT("/Game/")))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("destination_folder must start with /Game/ (got %s)"), *DestFolder),
			EMCPErrorCode::InvalidPath,
			TEXT("`destination_folder` must be a `/Game/...` content-root path. Filesystem paths (`C:/...`) and other content roots are not accepted by UAssetImportTask."));
	}
	// Strip trailing slash; UAssetImportTask wants "/Game/Foo", not "/Game/Foo/".
	DestFolder.RemoveFromEnd(TEXT("/"));

	const TArray<TSharedPtr<FJsonValue>>* Sounds = nullptr;
	if (!Params->TryGetArrayField(TEXT("sounds"), Sounds) || Sounds == nullptr || Sounds->Num() == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'sounds' parameter (must be a non-empty array)"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`sounds` is required and must be a non-empty JSON array of objects, each with at least `path` (filesystem path to a source audio file) and optionally `name` and `looping` (bool)."));
	}

	bool bForceOverwrite = true;
	Params->TryGetBoolField(TEXT("force_overwrite"), bForceOverwrite);

	TArray<UAssetImportTask*> Tasks;
	TArray<bool> Looping;
	TArray<TSharedPtr<FJsonValue>> Failures;

	for (const TSharedPtr<FJsonValue>& Entry : *Sounds)
	{
		const TSharedPtr<FJsonObject>* SndObj = nullptr;
		if (!Entry->TryGetObject(SndObj) || SndObj == nullptr)
		{
			continue;
		}
		const TSharedPtr<FJsonObject>& Snd = *SndObj;

		FString SrcPath;
		if (!Snd->TryGetStringField(TEXT("path"), SrcPath))
		{
			TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
			F->SetStringField(TEXT("path"), TEXT(""));
			F->SetStringField(TEXT("reason"), TEXT("missing 'path' field"));
			Failures.Add(MakeShared<FJsonValueObject>(F));
			continue;
		}
		const FString FullSrc = FPaths::ConvertRelativePathToFull(SrcPath);
		if (!FPaths::FileExists(FullSrc))
		{
			TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
			F->SetStringField(TEXT("path"), FullSrc);
			F->SetStringField(TEXT("reason"), TEXT("source file does not exist"));
			Failures.Add(MakeShared<FJsonValueObject>(F));
			continue;
		}

		FString AssetName;
		if (!Snd->TryGetStringField(TEXT("name"), AssetName) || AssetName.IsEmpty())
		{
			AssetName = FPaths::GetBaseFilename(FullSrc);
		}

		bool bLoop = false;
		Snd->TryGetBoolField(TEXT("looping"), bLoop);

		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->AddToRoot(); // protect from GC across the import call
		Task->Filename         = FullSrc;
		Task->DestinationPath  = DestFolder;
		Task->DestinationName  = AssetName;
		Task->bAutomated       = true;
		Task->bReplaceExisting = bForceOverwrite;
		Task->bSave            = false; // we save after applying the looping flag
		Task->Factory          = NewObject<USoundFactory>();

		Tasks.Add(Task);
		Looping.Add(bLoop);
	}

	if (Tasks.Num() == 0)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("count"), 0);
		Data->SetArrayField(TEXT("imported"), TArray<TSharedPtr<FJsonValue>>());
		Data->SetArrayField(TEXT("failed"), Failures);
		return Data;
	}

	FAssetToolsModule& AssetToolsModule =
		FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	AssetToolsModule.Get().ImportAssetTasks(Tasks);

	TArray<TSharedPtr<FJsonValue>> Imported;

	for (int32 i = 0; i < Tasks.Num(); ++i)
	{
		UAssetImportTask* Task = Tasks[i];
		Task->RemoveFromRoot();

		if (Task->ImportedObjectPaths.Num() == 0)
		{
			TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
			F->SetStringField(TEXT("path"), Task->Filename);
			F->SetStringField(TEXT("reason"),
				TEXT("ImportAssetTasks produced no asset (USoundFactory rejected the file — unsupported format, or .mp3/.ogg/etc. on a build without WITH_SNDFILE_IO). Check the editor Output Log (LogAudio/LogFactory)."));
			Failures.Add(MakeShared<FJsonValueObject>(F));
			continue;
		}

		const FString AssetPath = Task->ImportedObjectPaths[0];
		USoundWave* Wave = Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *AssetPath));
		if (Wave == nullptr)
		{
			TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
			F->SetStringField(TEXT("path"), Task->Filename);
			F->SetStringField(TEXT("reason"), FString::Printf(TEXT("imported asset at %s is not a USoundWave"), *AssetPath));
			Failures.Add(MakeShared<FJsonValueObject>(F));
			continue;
		}

		Wave->PreEditChange(nullptr);
		Wave->bLooping = Looping[i];
		Wave->PostEditChange();
		Wave->MarkPackageDirty();

		// USoundWave imports queue an async DDC build (like textures). Saving mid-build can write an
		// incomplete package, so block on compilation before persisting — mirrors the texture importer's
		// FTextureCompilingManager barrier. Files here are small, so this returns quickly.
		FSoundWaveCompilingManager::Get().FinishCompilation({ Wave });

		// Save the live object directly (SaveLoadedAsset, not SaveAsset(path)) so the on-disk package
		// carries our bLooping flag even while the wave's async build settles.
		const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Wave, /*bOnlyIfIsDirty=*/ false);
		if (!bSaved)
		{
			TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
			F->SetStringField(TEXT("path"), Task->Filename);
			F->SetStringField(TEXT("reason"), FString::Printf(TEXT("SaveLoadedAsset returned false for %s; asset was imported in-memory but NOT written to disk (PIE session live, asset unregistered, or save failure) and will be GC'd"), *AssetPath));
			Failures.Add(MakeShared<FJsonValueObject>(F));
			continue;
		}

		TSharedPtr<FJsonObject> EntryOut = MakeShared<FJsonObject>();
		EntryOut->SetStringField(TEXT("asset_path"), AssetPath);
		EntryOut->SetBoolField(TEXT("looping"), Wave->bLooping);
		Imported.Add(MakeShared<FJsonValueObject>(EntryOut));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), Imported.Num());
	Data->SetArrayField(TEXT("imported"), Imported);
	Data->SetArrayField(TEXT("failed"), Failures);
	return Data;
}
