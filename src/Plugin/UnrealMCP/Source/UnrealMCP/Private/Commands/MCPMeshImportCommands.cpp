#include "Commands/MCPMeshImportCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "EditorAssetLibrary.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

FMCPMeshImportCommands::FMCPMeshImportCommands()
{
}

TSharedPtr<FJsonObject> FMCPMeshImportCommands::HandleCommand(
	const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("asset_import_mesh"))
	{
		return HandleImportMesh(Params);
	}
	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown mesh-import command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be one of: asset_import_mesh."));
}

TSharedPtr<FJsonObject> FMCPMeshImportCommands::HandleImportMesh(
	const TSharedPtr<FJsonObject>& Params)
{
	// ── Validate source file ──────────────────────────────────────────────────
	FString SrcPath;
	if (!Params->TryGetStringField(TEXT("source_path"), SrcPath) || SrcPath.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'source_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`source_path` is required and must be an absolute filesystem path to a mesh file (.fbx/.obj/.gltf), e.g. `C:/Assets/bike.fbx`."));
	}
	const FString FullSrc = FPaths::ConvertRelativePathToFull(SrcPath);
	if (!FPaths::FileExists(FullSrc))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Source file does not exist: %s"), *FullSrc),
			EMCPErrorCode::InvalidPath,
			TEXT("`source_path` must point to an existing file on disk. Check the path and that the process has read access to it."));
	}

	// ── Validate destination folder ───────────────────────────────────────────
	FString DestFolder;
	if (!Params->TryGetStringField(TEXT("destination_folder"), DestFolder))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'destination_folder' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`destination_folder` is required and must be a `/Game/...` content-root path (no `.uasset` suffix), e.g. `/Game/Meshes/Imported`. The folder is created if it doesn't exist."));
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

	FString AssetName;
	if (!Params->TryGetStringField(TEXT("name"), AssetName) || AssetName.IsEmpty())
	{
		AssetName = FPaths::GetBaseFilename(FullSrc);
	}

	bool bImportAsSkeletal = false;
	Params->TryGetBoolField(TEXT("import_as_skeletal"), bImportAsSkeletal);
	bool bImportMaterials = true;
	Params->TryGetBoolField(TEXT("import_materials"), bImportMaterials);
	bool bImportTextures = true;
	Params->TryGetBoolField(TEXT("import_textures"), bImportTextures);
	bool bCombineMeshes = false;
	Params->TryGetBoolField(TEXT("combine_meshes"), bCombineMeshes);
	bool bForceOverwrite = true;
	Params->TryGetBoolField(TEXT("force_overwrite"), bForceOverwrite);

	// ── Resolve an optional skeleton for skeletal imports ─────────────────────
	USkeleton* Skeleton = nullptr;
	FString SkeletonPath;
	if (Params->TryGetStringField(TEXT("skeleton"), SkeletonPath) && !SkeletonPath.IsEmpty())
	{
		Skeleton = Cast<USkeleton>(UEditorAssetLibrary::LoadAsset(SkeletonPath));
		if (Skeleton == nullptr)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("skeleton asset not found or not a USkeleton: %s"), *SkeletonPath),
				EMCPErrorCode::AssetNotFound,
				TEXT("`skeleton` must be a `/Game/...` path to an existing USkeleton. Omit it to let the importer create a new skeleton from the FBX rig."));
		}
	}

	// ── Configure the FBX import options ──────────────────────────────────────
	// We attach a UFbxImportUI as Task->Options and force UFbxFactory so these
	// settings are honored deterministically in automated mode (the same machinery
	// the texture importer uses with UTextureFactory). The factory auto-handles
	// .fbx/.obj/.gltf source extensions.
	UFbxImportUI* ImportUI = NewObject<UFbxImportUI>();
	ImportUI->AddToRoot(); // protect from GC across the import call
	ImportUI->bImportMesh          = true;
	ImportUI->bImportMaterials     = bImportMaterials;
	ImportUI->bImportTextures      = bImportTextures;
	ImportUI->bImportAsSkeletal    = bImportAsSkeletal;
	ImportUI->bImportAnimations    = false; // mesh import only — animations are a separate concern
	ImportUI->MeshTypeToImport     = bImportAsSkeletal ? FBXIT_SkeletalMesh : FBXIT_StaticMesh;
	ImportUI->OriginalImportType   = ImportUI->MeshTypeToImport;
	// We set the type explicitly, so don't let the factory re-sniff it.
	ImportUI->bAutomatedImportShouldDetectType = false;
	if (Skeleton != nullptr)
	{
		ImportUI->Skeleton = Skeleton;
	}
	if (ImportUI->StaticMeshImportData != nullptr)
	{
		ImportUI->StaticMeshImportData->bCombineMeshes = bCombineMeshes;
	}

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->AddToRoot(); // protect from GC across the import call
	Task->Filename         = FullSrc;
	Task->DestinationPath  = DestFolder;
	Task->DestinationName  = AssetName;
	Task->bAutomated       = true;
	Task->bReplaceExisting = bForceOverwrite;
	Task->bSave            = true;
	Task->Factory          = NewObject<UFbxFactory>();
	Task->Options          = ImportUI;

	FAssetToolsModule& AssetToolsModule =
		FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

	TArray<UAssetImportTask*> Tasks;
	Tasks.Add(Task);
	AssetToolsModule.Get().ImportAssetTasks(Tasks);

	TArray<TSharedPtr<FJsonValue>> Imported;
	TArray<TSharedPtr<FJsonValue>> CreatedMaterials;
	TArray<TSharedPtr<FJsonValue>> CreatedTextures;
	TArray<TSharedPtr<FJsonValue>> Failures;

	Task->RemoveFromRoot();
	ImportUI->RemoveFromRoot();

	if (Task->ImportedObjectPaths.Num() == 0)
	{
		TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
		F->SetStringField(TEXT("path"), Task->Filename);
		F->SetStringField(TEXT("reason"),
			TEXT("ImportAssetTasks produced no asset (the FBX factory rejected the file, or import_as_skeletal was set on a file with no skeleton/skin). Check the editor Output Log (LogFbx) for details."));
		Failures.Add(MakeShared<FJsonValueObject>(F));
	}
	else
	{
		// Classify every produced asset: the primary mesh(es) under imported[],
		// and any embedded materials/textures under their own lists.
		for (const FString& AssetPath : Task->ImportedObjectPaths)
		{
			UObject* Obj = UEditorAssetLibrary::LoadAsset(AssetPath);
			if (Obj == nullptr)
			{
				continue;
			}

			if (Obj->IsA(UStaticMesh::StaticClass()) || Obj->IsA(USkeletalMesh::StaticClass()))
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("source"), Task->Filename);
				Entry->SetStringField(TEXT("asset_path"), AssetPath);
				Entry->SetStringField(TEXT("class"), Obj->GetClass()->GetName());
				Imported.Add(MakeShared<FJsonValueObject>(Entry));
			}
			else if (Obj->IsA(UMaterialInterface::StaticClass()))
			{
				CreatedMaterials.Add(MakeShared<FJsonValueString>(AssetPath));
			}
			else if (Obj->IsA(UTexture::StaticClass()))
			{
				CreatedTextures.Add(MakeShared<FJsonValueString>(AssetPath));
			}
		}

		if (Imported.Num() == 0)
		{
			TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
			F->SetStringField(TEXT("path"), Task->Filename);
			F->SetStringField(TEXT("reason"),
				FString::Printf(TEXT("import produced %d asset(s) but none were a UStaticMesh/USkeletalMesh (only materials/textures). Verify the FBX contains mesh geometry and that import_as_skeletal matches the file."),
					Task->ImportedObjectPaths.Num()));
			Failures.Add(MakeShared<FJsonValueObject>(F));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), Imported.Num());
	Data->SetArrayField(TEXT("imported"), Imported);
	Data->SetArrayField(TEXT("created_materials"), CreatedMaterials);
	Data->SetArrayField(TEXT("created_textures"), CreatedTextures);
	Data->SetArrayField(TEXT("failed"), Failures);
	return Data;
}
