#include "Commands/MCPTextureCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/Texture2D.h"
#include "Factories/TextureFactory.h"
#include "TextureCompiler.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

FMCPTextureCommands::FMCPTextureCommands()
{
}

TSharedPtr<FJsonObject> FMCPTextureCommands::HandleCommand(
	const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("asset_textures_import"))
	{
		return HandleImportTextures(Params);
	}
	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown texture command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be one of: import_textures."));
}

namespace
{
	TextureCompressionSettings ParseCompression(const FString& In)
	{
		if (In.Equals(TEXT("Default"),         ESearchCase::IgnoreCase)) return TC_Default;
		if (In.Equals(TEXT("NormalMap"),       ESearchCase::IgnoreCase)) return TC_Normalmap;
		if (In.Equals(TEXT("Masks"),           ESearchCase::IgnoreCase)) return TC_Masks;
		if (In.Equals(TEXT("Grayscale"),       ESearchCase::IgnoreCase)) return TC_Grayscale;
		if (In.Equals(TEXT("Displacementmap"), ESearchCase::IgnoreCase)) return TC_Displacementmap;
		if (In.Equals(TEXT("HDR"),             ESearchCase::IgnoreCase)) return TC_HDR;
		if (In.Equals(TEXT("EditorIcon"),      ESearchCase::IgnoreCase)) return TC_EditorIcon;
		if (In.Equals(TEXT("Alpha"),           ESearchCase::IgnoreCase)) return TC_Alpha;
		// Caller passed an unknown literal — preserve their intent in logs and fall back.
		UE_LOG(LogUnrealMCP, Warning, TEXT("import_textures: unknown compression '%s', using TC_Default"), *In);
		return TC_Default;
	}

	const TCHAR* CompressionLabel(TextureCompressionSettings TC)
	{
		switch (TC)
		{
			case TC_Default:         return TEXT("Default");
			case TC_Normalmap:       return TEXT("NormalMap");
			case TC_Masks:           return TEXT("Masks");
			case TC_Grayscale:       return TEXT("Grayscale");
			case TC_Displacementmap: return TEXT("Displacementmap");
			case TC_HDR:             return TEXT("HDR");
			case TC_EditorIcon:      return TEXT("EditorIcon");
			case TC_Alpha:           return TEXT("Alpha");
			default:                 return TEXT("Other");
		}
	}

	TextureGroup ParseLODGroup(const FString& In)
	{
		if (In.Equals(TEXT("World"),     ESearchCase::IgnoreCase)) return TEXTUREGROUP_World;
		if (In.Equals(TEXT("UI"),        ESearchCase::IgnoreCase)) return TEXTUREGROUP_UI;
		if (In.Equals(TEXT("Effects"),   ESearchCase::IgnoreCase)) return TEXTUREGROUP_Effects;
		if (In.Equals(TEXT("Skybox"),    ESearchCase::IgnoreCase)) return TEXTUREGROUP_Skybox;
		if (In.Equals(TEXT("Character"), ESearchCase::IgnoreCase)) return TEXTUREGROUP_Character;
		if (In.Equals(TEXT("Weapon"),    ESearchCase::IgnoreCase)) return TEXTUREGROUP_Weapon;
		if (In.Equals(TEXT("Vehicle"),   ESearchCase::IgnoreCase)) return TEXTUREGROUP_Vehicle;
		if (In.Equals(TEXT("Pixels2D"),  ESearchCase::IgnoreCase)) return TEXTUREGROUP_Pixels2D;
		UE_LOG(LogUnrealMCP, Warning, TEXT("import_textures: unknown lod_group '%s', using World"), *In);
		return TEXTUREGROUP_World;
	}

	const TCHAR* LODGroupLabel(TextureGroup G)
	{
		switch (G)
		{
			case TEXTUREGROUP_World:     return TEXT("World");
			case TEXTUREGROUP_UI:        return TEXT("UI");
			case TEXTUREGROUP_Effects:   return TEXT("Effects");
			case TEXTUREGROUP_Skybox:    return TEXT("Skybox");
			case TEXTUREGROUP_Character: return TEXT("Character");
			case TEXTUREGROUP_Weapon:    return TEXT("Weapon");
			case TEXTUREGROUP_Vehicle:   return TEXT("Vehicle");
			case TEXTUREGROUP_Pixels2D:  return TEXT("Pixels2D");
			default:                     return TEXT("Other");
		}
	}

	TextureMipGenSettings ParseMipGen(const FString& In)
	{
		if (In.Equals(TEXT("FromTextureGroup"),  ESearchCase::IgnoreCase)) return TMGS_FromTextureGroup;
		if (In.Equals(TEXT("NoMipmaps"),         ESearchCase::IgnoreCase)) return TMGS_NoMipmaps;
		if (In.Equals(TEXT("SimpleAverage"),     ESearchCase::IgnoreCase)) return TMGS_SimpleAverage;
		if (In.Equals(TEXT("LeaveExistingMips"), ESearchCase::IgnoreCase)) return TMGS_LeaveExistingMips;
		UE_LOG(LogUnrealMCP, Warning, TEXT("import_textures: unknown mip_gen '%s', using FromTextureGroup"), *In);
		return TMGS_FromTextureGroup;
	}

	const TCHAR* MipGenLabel(TextureMipGenSettings M)
	{
		switch (M)
		{
			case TMGS_FromTextureGroup:  return TEXT("FromTextureGroup");
			case TMGS_NoMipmaps:         return TEXT("NoMipmaps");
			case TMGS_SimpleAverage:     return TEXT("SimpleAverage");
			case TMGS_LeaveExistingMips: return TEXT("LeaveExistingMips");
			default:                     return TEXT("Other");
		}
	}

	ECompositeTextureMode ParseCompositeMode(const FString& In)
	{
		if (In.Equals(TEXT("NormalRoughnessToRed"),   ESearchCase::IgnoreCase)) return CTM_NormalRoughnessToRed;
		if (In.Equals(TEXT("NormalRoughnessToGreen"), ESearchCase::IgnoreCase)) return CTM_NormalRoughnessToGreen;
		if (In.Equals(TEXT("NormalRoughnessToBlue"),  ESearchCase::IgnoreCase)) return CTM_NormalRoughnessToBlue;
		if (In.Equals(TEXT("NormalRoughnessToAlpha"), ESearchCase::IgnoreCase)) return CTM_NormalRoughnessToAlpha;
		if (In.Equals(TEXT("Disabled"),               ESearchCase::IgnoreCase)) return CTM_Disabled;
		UE_LOG(LogUnrealMCP, Warning, TEXT("import_textures: unknown composite_mode '%s', leaving disabled"), *In);
		return CTM_Disabled;
	}

	// Resolve a /Game/... path to a UTexture, with the same fallback the rest of
	// the plugin uses (LoadAsset → LoadObject with .AssetName suffix). Needed for
	// composite_texture references which may arrive with or without the suffix.
	UTexture* ResolveTextureByPath(const FString& Path)
	{
		if (Path.IsEmpty()) return nullptr;
		UTexture* Found = Cast<UTexture>(UEditorAssetLibrary::LoadAsset(Path));
		if (Found == nullptr)
		{
			const FString FullPath = Path.Contains(TEXT("."))
				? Path
				: FString::Printf(TEXT("%s.%s"), *Path, *FPaths::GetBaseFilename(Path));
			Found = Cast<UTexture>(StaticLoadObject(UTexture::StaticClass(), nullptr, *FullPath));
		}
		return Found;
	}

	// Per-image staged settings — applied after ImportAssetTasks builds the
	// UTexture2D, before we save. UTextureFactory has no way to thread these
	// through at import time.
	struct FStagedSettings
	{
		bool                       bSRGB       = true;
		TextureCompressionSettings Compression = TC_Default;
		// LOD group / mip gen are only applied when the caller sent them — the
		// factory defaults (World / FromTextureGroup) stay otherwise. UI-group is
		// load-bearing for HUD art: UI textures are never streamed, so the full
		// mip is always resident (a World-group HUD texture renders a blurry low
		// mip on first open until streaming catches up).
		bool                       bHasLODGroup = false;
		TextureGroup               LODGroup     = TEXTUREGROUP_World;
		bool                       bHasMipGen   = false;
		TextureMipGenSettings      MipGen       = TMGS_FromTextureGroup;
		// Composite texture (Toksvig / LEAN-style normal-variance → roughness mips).
		// When set, UE bakes the variance lost by averaging the composite source's
		// normals at each mip level into this texture's specified channel at the
		// matching mip level — distant rough surfaces stay rough instead of
		// going unrealistically smooth. See:
		// https://docs.unrealengine.com/5.7/Engine/Content/Types/Textures/Properties/
		FString                    CompositeTexturePath;  // empty = no composite
		ECompositeTextureMode      CompositeMode = CTM_Disabled;
	};
}

TSharedPtr<FJsonObject> FMCPTextureCommands::HandleImportTextures(
	const TSharedPtr<FJsonObject>& Params)
{
	FString DestFolder;
	if (!Params->TryGetStringField(TEXT("destination_folder"), DestFolder))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'destination_folder' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`destination_folder` is required and must be a `/Game/...` content-root path (no `.uasset` suffix), e.g. `/Game/Textures/Imported`. The folder is created if it doesn't exist."));
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

	const TArray<TSharedPtr<FJsonValue>>* Images = nullptr;
	if (!Params->TryGetArrayField(TEXT("images"), Images) || Images == nullptr || Images->Num() == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'images' parameter (must be a non-empty array)"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`images` is required and must be a non-empty JSON array of objects, each with at least `path` (filesystem path to a source image file) and optionally `name`, `settings.sRGB`, `settings.compression`, `settings.lod_group`, `settings.mip_gen`, `settings.composite_texture`, `settings.composite_mode`."));
	}

	bool bForceOverwrite = true;
	Params->TryGetBoolField(TEXT("force_overwrite"), bForceOverwrite);

	TArray<UAssetImportTask*> Tasks;
	TArray<FStagedSettings> Staged;
	TArray<TSharedPtr<FJsonValue>> Failures;

	for (const TSharedPtr<FJsonValue>& Entry : *Images)
	{
		const TSharedPtr<FJsonObject>* ImgObj = nullptr;
		if (!Entry->TryGetObject(ImgObj) || ImgObj == nullptr)
		{
			continue;
		}
		const TSharedPtr<FJsonObject>& Img = *ImgObj;

		FString SrcPath;
		if (!Img->TryGetStringField(TEXT("path"), SrcPath))
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
		if (!Img->TryGetStringField(TEXT("name"), AssetName) || AssetName.IsEmpty())
		{
			AssetName = FPaths::GetBaseFilename(FullSrc);
		}

		FStagedSettings S;
		const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
		if (Img->TryGetObjectField(TEXT("settings"), SettingsObj) && SettingsObj != nullptr)
		{
			(*SettingsObj)->TryGetBoolField(TEXT("sRGB"), S.bSRGB);
			FString CompStr;
			if ((*SettingsObj)->TryGetStringField(TEXT("compression"), CompStr))
			{
				S.Compression = ParseCompression(CompStr);
			}
			FString LODGroupStr;
			if ((*SettingsObj)->TryGetStringField(TEXT("lod_group"), LODGroupStr))
			{
				S.bHasLODGroup = true;
				S.LODGroup = ParseLODGroup(LODGroupStr);
			}
			FString MipGenStr;
			if ((*SettingsObj)->TryGetStringField(TEXT("mip_gen"), MipGenStr))
			{
				S.bHasMipGen = true;
				S.MipGen = ParseMipGen(MipGenStr);
			}
			(*SettingsObj)->TryGetStringField(TEXT("composite_texture"), S.CompositeTexturePath);
			FString CompModeStr;
			if ((*SettingsObj)->TryGetStringField(TEXT("composite_mode"), CompModeStr))
			{
				S.CompositeMode = ParseCompositeMode(CompModeStr);
			}
		}

		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->AddToRoot(); // protect from GC across the import call
		Task->Filename          = FullSrc;
		Task->DestinationPath   = DestFolder;
		Task->DestinationName   = AssetName;
		Task->bAutomated        = true;
		Task->bReplaceExisting  = bForceOverwrite;
		Task->bSave             = false; // we save after applying settings
		Task->Factory           = NewObject<UTextureFactory>();

		Tasks.Add(Task);
		Staged.Add(S);
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
			F->SetStringField(TEXT("reason"), TEXT("ImportAssetTasks produced no asset (factory rejected the file?)"));
			Failures.Add(MakeShared<FJsonValueObject>(F));
			continue;
		}

		const FString AssetPath = Task->ImportedObjectPaths[0];
		UTexture2D* Tex = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AssetPath));
		if (Tex == nullptr)
		{
			TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
			F->SetStringField(TEXT("path"), Task->Filename);
			F->SetStringField(TEXT("reason"), FString::Printf(TEXT("imported asset at %s is not a UTexture2D"), *AssetPath));
			Failures.Add(MakeShared<FJsonValueObject>(F));
			continue;
		}

		Tex->PreEditChange(nullptr);
		Tex->SRGB = Staged[i].bSRGB;
		Tex->CompressionSettings = Staged[i].Compression;
		if (Staged[i].bHasLODGroup)
		{
			Tex->LODGroup = Staged[i].LODGroup;
		}
		if (Staged[i].bHasMipGen)
		{
			Tex->MipGenSettings = Staged[i].MipGen;
		}

		// Composite Texture (normal-variance → roughness mips). Resolved AFTER
		// the factory imports the source — caller must order images so the
		// composite source (typically the normal map) appears earlier in the
		// images[] array than its referrers (orm, roughness).
		if (!Staged[i].CompositeTexturePath.IsEmpty() && Staged[i].CompositeMode != CTM_Disabled)
		{
			UTexture* CompositeTex = ResolveTextureByPath(Staged[i].CompositeTexturePath);
			if (CompositeTex == nullptr)
			{
				UE_LOG(LogUnrealMCP, Warning,
					TEXT("import_textures: composite_texture '%s' did not resolve for %s — skipping composite"),
					*Staged[i].CompositeTexturePath, *AssetPath);
			}
			else
			{
				Tex->CompositeTexture = CompositeTex;
				Tex->CompositeTextureMode = Staged[i].CompositeMode;
			}
		}

		Tex->PostEditChange();
		Tex->MarkPackageDirty();

		// Block until async texture compilation finishes. UE5 queues the texture
		// build after PostEditChange; saving (or path-reloading) it mid-build is a
		// no-op/failure, so without this large textures (e.g. 1024×1536) are never
		// written to disk and the unrooted in-memory object is then GC'd —
		// import "succeeds" but nothing persists. Small icons compile fast enough
		// to dodge this, which is why it only bites larger imports.
		FTextureCompilingManager::Get().FinishCompilation({ Tex });

		// Save the *live* object directly. SaveAsset(path) re-resolves via LoadAsset,
		// which returns null while the texture is still compiling; SaveLoadedAsset
		// uses the object we already hold, so the on-disk package is written with
		// our applied settings (sRGB / compression).
		const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Tex, /*bOnlyIfIsDirty=*/ false);
		if (!bSaved)
		{
			TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
			F->SetStringField(TEXT("path"), Task->Filename);
			F->SetStringField(TEXT("reason"), FString::Printf(TEXT("SaveLoadedAsset returned false for %s; asset was imported in-memory but NOT written to disk (PIE session live, asset unregistered, or checkout/save failure) and will be GC'd"), *AssetPath));
			Failures.Add(MakeShared<FJsonValueObject>(F));
			continue;
		}

		// Source.GetSizeX/Y returns the import-time bitmap dimensions and is
		// always immediate. GetSizeX/Y on the texture itself reads from
		// PlatformData, which the PostEditChange above kicks into an async
		// rebuild — until that completes, the values lag (we saw 32×32
		// reported for 2048×2048 sources after switching CompressionSettings
		// to Masks/Grayscale/Displacementmap; the actual asset was correct
		// once the rebuild finished, only the response was stale).
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), AssetPath);
		Entry->SetNumberField(TEXT("size_x"), static_cast<int64>(Tex->Source.GetSizeX()));
		Entry->SetNumberField(TEXT("size_y"), static_cast<int64>(Tex->Source.GetSizeY()));
		Entry->SetBoolField(TEXT("sRGB"), Tex->SRGB);
		Entry->SetStringField(TEXT("compression"), CompressionLabel(Tex->CompressionSettings));
		Entry->SetStringField(TEXT("lod_group"), LODGroupLabel(Tex->LODGroup));
		Entry->SetStringField(TEXT("mip_gen"), MipGenLabel(Tex->MipGenSettings));
		if (Tex->CompositeTexture != nullptr && Tex->CompositeTextureMode != CTM_Disabled)
		{
			Entry->SetStringField(TEXT("composite_texture"), Tex->CompositeTexture->GetPathName());
			const TCHAR* ModeLabel = TEXT("Other");
			switch (Tex->CompositeTextureMode)
			{
				case CTM_NormalRoughnessToRed:   ModeLabel = TEXT("NormalRoughnessToRed");   break;
				case CTM_NormalRoughnessToGreen: ModeLabel = TEXT("NormalRoughnessToGreen"); break;
				case CTM_NormalRoughnessToBlue:  ModeLabel = TEXT("NormalRoughnessToBlue");  break;
				case CTM_NormalRoughnessToAlpha: ModeLabel = TEXT("NormalRoughnessToAlpha"); break;
				default: break;
			}
			Entry->SetStringField(TEXT("composite_mode"), ModeLabel);
		}
		Imported.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), Imported.Num());
	Data->SetArrayField(TEXT("imported"), Imported);
	Data->SetArrayField(TEXT("failed"), Failures);
	return Data;
}
