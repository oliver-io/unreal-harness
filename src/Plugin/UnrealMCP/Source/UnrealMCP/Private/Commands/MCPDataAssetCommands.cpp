#include "Commands/MCPDataAssetCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "Engine/DataAsset.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "JsonObjectConverter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/Field.h"
#include "Misc/Paths.h"


// ── Dispatch ────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPDataAssetCommands::HandleCommand(
	const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("asset_dataasset_create"))         return HandleCreateDataAsset(Params);
	if (CommandType == TEXT("asset_dataasset_set_property"))   return HandleSetDataAssetProperty(Params);
	if (CommandType == TEXT("asset_dataasset_read"))           return HandleReadDataAsset(Params);

	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown data-asset command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be one of: create_data_asset, set_data_asset_property, read_data_asset."));
}

// ── Helpers ─────────────────────────────────────────────────────────────────

UClass* FMCPDataAssetCommands::ResolveDataAssetClass(const FString& ClassName)
{
	if (ClassName.IsEmpty()) return nullptr;

	UClass* Found = nullptr;

	// Strategy 1: FQN path /Script/Module.Class
	if (ClassName.Contains(TEXT("/")) || ClassName.Contains(TEXT(".")))
	{
		Found = LoadClass<UObject>(nullptr, *ClassName);
	}
	else
	{
		// Strategy 2/3: short name with optional U/A prefix strip.
		// "WarmupProfile", "UWarmupProfile", "AWarmupProfile" all resolve.
		FString Stripped = ClassName;
		if (Stripped.Len() > 1 &&
		    (Stripped[0] == TEXT('U') || Stripped[0] == TEXT('A')) &&
		    FChar::IsUpper(Stripped[1]))
		{
			Stripped = Stripped.RightChop(1);
		}

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Cls = *It;
			const FString CName = Cls->GetName();
			if (CName == ClassName || CName == Stripped)
			{
				Found = Cls;
				break;
			}
		}
	}

	if (Found && Found->IsChildOf(UDataAsset::StaticClass()))
	{
		return Found;
	}
	return nullptr;
}

UDataAsset* FMCPDataAssetCommands::LoadDataAssetByPath(const FString& AssetPath)
{
	UObject* Loaded = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!Loaded)
	{
		// LoadAsset's normalisation can miss leading-underscore paths or
		// odd casing — fall back to LoadObject with .AssetName suffix.
		const FString AssetName = FPaths::GetBaseFilename(AssetPath);
		const FString FullPath = AssetPath.Contains(TEXT("."))
			? AssetPath
			: FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
		Loaded = LoadObject<UObject>(nullptr, *FullPath);
	}
	return Cast<UDataAsset>(Loaded);
}

FProperty* FMCPDataAssetCommands::FindEditableProperty(UDataAsset* Asset, const FName& PropertyName)
{
	if (!Asset) return nullptr;
	FProperty* Prop = Asset->GetClass()->FindPropertyByName(PropertyName);
	if (!Prop) return nullptr;

	// Reject writes to non-edit-exposed properties — keeps the surface
	// honest to the UPROPERTY contract.  EditAnywhere / EditDefaultsOnly
	// both set CPF_Edit.  BlueprintReadWrite alone (without Edit) also
	// signals intentional external mutation, so we accept it too.
	const uint64 RequiredAny = CPF_Edit | CPF_BlueprintAssignable | CPF_BlueprintVisible;
	if ((Prop->PropertyFlags & RequiredAny) == 0)
	{
		return nullptr;
	}
	return Prop;
}

// ── create_data_asset ──────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPDataAssetCommands::HandleCreateDataAsset(
	const TSharedPtr<FJsonObject>& Params)
{
	FString NameParam;
	if (!Params->TryGetStringField(TEXT("name"), NameParam))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`name` is required. Pass either a short asset name (lands at `/Game/DataAssets/<name>`) or a full `/Game/Folder/AssetName` path. The folder is created if missing."));
	}

	FString ClassParam;
	if (!Params->TryGetStringField(TEXT("class"), ClassParam))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'class' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`class` is required and must name a UDataAsset subclass. Accepted forms: short name (`WarmupProfile`), prefixed (`UWarmupProfile`), or FQN path (`/Script/Module.UClassName`). Use `get_class_properties` on a candidate to verify it derives from UDataAsset."));
	}

	// ── Path handling — mirrors HandleCreateBlueprint ──────────────────────
	FString PackagePath;
	FString AssetName;
	if (NameParam.StartsWith(TEXT("/")))
	{
		int32 LastSlash = INDEX_NONE;
		NameParam.FindLastChar(TEXT('/'), LastSlash);
		if (LastSlash <= 0)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Malformed asset path: '%s'. Expected '/Game/Folder/AssetName'."), *NameParam),
				EMCPErrorCode::InvalidPath,
				TEXT("Path-form `name` must include at least one folder segment between the leading `/` and the asset name, e.g. `/Game/DataAssets/MyAsset`. A bare `/AssetName` is not valid — pass `MyAsset` instead (defaults to `/Game/DataAssets`) or supply the full path."));
		}
		PackagePath = NameParam.Left(LastSlash);
		AssetName   = NameParam.Mid(LastSlash + 1);
	}
	else
	{
		PackagePath = TEXT("/Game/DataAssets");
		AssetName   = NameParam;
	}

	const FString FullAssetPath = PackagePath / AssetName;

	bool bForceOverwrite = false;
	Params->TryGetBoolField(TEXT("force_overwrite"), bForceOverwrite);

	if (UEditorAssetLibrary::DoesAssetExist(FullAssetPath))
	{
		if (bForceOverwrite)
		{
			UEditorAssetLibrary::DeleteAsset(FullAssetPath);
		}
		else
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Asset already exists: %s — pass force_overwrite=true to replace"), *FullAssetPath),
				EMCPErrorCode::NameCollision,
				TEXT("An asset already exists at the target path. To replace it, pass `force_overwrite: true` (the existing asset is deleted first). To keep the existing asset, pick a different name or path. Use `read_data_asset` to inspect the existing asset before overwriting."));
		}
	}

	// ── Class resolution ───────────────────────────────────────────────────
	UClass* AssetClass = ResolveDataAssetClass(ClassParam);
	if (!AssetClass)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Unresolved data-asset class '%s' (must derive from UDataAsset)"), *ClassParam),
			EMCPErrorCode::ClassNotLoaded,
			TEXT("`class` did not resolve to a UDataAsset subclass. Resolver accepts: FQN paths (`/Script/Module.Class`), short names with optional `U`/`A` prefix strip (`WarmupProfile`, `UWarmupProfile`). The class must derive from UDataAsset and be loaded — for C++ classes, ensure the module is loaded; for Blueprints, the BP must be compiled and resident in memory."));
	}

	if (AssetClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Data-asset class '%s' is abstract and cannot be instantiated"), *AssetClass->GetName()),
			EMCPErrorCode::InvalidArgument,
			TEXT("The resolved class is marked abstract. Pick a concrete UDataAsset subclass — instantiating an abstract class fires an editor ensure and produces an asset that is nulled out on save."));
	}

	// ── Instantiate ────────────────────────────────────────────────────────
	UPackage* Package = CreatePackage(*FullAssetPath);
	if (!Package)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to create package: %s"), *FullAssetPath),
			EMCPErrorCode::Internal,
			TEXT("CreatePackage returned nullptr for the resolved asset path. Typically a path-validity issue beyond what the malformed-path check caught — verify the path is a `/Game/...` content-root path, contains no invalid filename characters, and isn't under a read-only source-control region. Retry with a known-good `/Game/DataAssets/...` path."));
	}

	UDataAsset* NewAsset = NewObject<UDataAsset>(
		Package, AssetClass, FName(*AssetName),
		RF_Public | RF_Standalone);
	if (!NewAsset)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to instantiate data asset"),
			EMCPErrorCode::Internal,
			TEXT("NewObject<UDataAsset> returned nullptr despite a valid resolved class and package. This typically indicates the class is abstract, has a CDO failure, or its constructor threw. Check `get_class_properties` on the class for a `bAbstract` flag and verify it can be default-constructed."));
	}

	FAssetRegistryModule::AssetCreated(NewAsset);
	NewAsset->MarkPackageDirty();
	if (!UEditorAssetLibrary::SaveAsset(FullAssetPath, /*bOnlyIfIsDirty=*/false))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Data asset created in memory but failed to save to disk: %s"), *FullAssetPath),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was created, registered and dirtied in memory but not written to disk (source-control checkout failure, read-only file, or a no-op save path). The asset exists for this editor session; resolve the save blocker and re-save, or use a SavePackages-based save path."));
	}

	UE_LOG(LogUnrealMCP, Display, TEXT("Created data asset: %s (class=%s)"),
		*FullAssetPath, *AssetClass->GetName());

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), FullAssetPath);
	Result->SetStringField(TEXT("class"), AssetClass->GetPathName());
	return Result;
}

// ── set_data_asset_property ────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPDataAssetCommands::HandleSetDataAssetProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'asset_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`asset_path` is required and must be the full asset path to a UDataAsset, e.g. `/Game/DataAssets/MyAsset`. Use `list_assets` with `asset_type='DataAsset'` to discover existing data assets."));
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property"), PropertyName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'property' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`property` is required (string, the FProperty name on the data-asset class). Property names are case-sensitive and must match the C++ UPROPERTY identifier. Use `read_data_asset` to enumerate the asset's edit-exposed properties."));
	}

	UDataAsset* Asset = LoadDataAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Asset not found or not a UDataAsset: %s"), *AssetPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`asset_path` either doesn't exist or resolves to a UObject that is not a UDataAsset. Verify the path with `list_assets` (asset_type='DataAsset'). Paths are case-sensitive and must include `/Game/` prefix."));
	}

	FProperty* Property = FindEditableProperty(Asset, FName(*PropertyName));
	if (!Property)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Property '%s' not found on %s, or not edit-exposed"), *PropertyName, *Asset->GetClass()->GetName()),
			EMCPErrorCode::InvalidArgument,
			TEXT("`property` either doesn't exist on the class or lacks the EditAnywhere / EditDefaultsOnly / BlueprintReadWrite / BlueprintAssignable / BlueprintVisible specifier required for external mutation. Names are case-sensitive. Use `read_data_asset` (lists edit-exposed properties) or `get_class_properties` (includes non-exposed) to verify."));
	}

	// Action defaults to "set" — covers the common case for both arrays
	// (replace whole) and non-arrays (assign).
	FString Action = TEXT("set");
	Params->TryGetStringField(TEXT("action"), Action);
	Action = Action.ToLower();

	FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property);
	void* PropertyData = Property->ContainerPtrToValuePtr<void>(Asset);

	// ── Modify ─────────────────────────────────────────────────────────────
	int32 NewArraySize = INDEX_NONE;

	if (ArrayProp)
	{
		FScriptArrayHelper Helper(ArrayProp, PropertyData);

		if (Action == TEXT("clear"))
		{
			Helper.EmptyValues();
			NewArraySize = 0;
		}
		else if (Action == TEXT("remove_at"))
		{
			int32 Index = INDEX_NONE;
			if (!Params->TryGetNumberField(TEXT("index"), Index) || Index < 0 || Index >= Helper.Num())
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("remove_at requires valid 'index' in [0, %d)"), Helper.Num()),
					EMCPErrorCode::OutOfRange,
					TEXT("`remove_at` action requires `index` in the half-open range [0, current_array_size). Use `read_data_asset` to see the array's current size, then pick a valid index. The bound moves after each remove — re-read if you're chaining removes."));
			}
			Helper.RemoveValues(Index, 1);
			NewArraySize = Helper.Num();
		}
		else if (Action == TEXT("append"))
		{
			TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(TEXT("value"));
			if (!JsonValue.IsValid())
			{
				return FMCPCommonUtils::CreateErrorResponse(
					TEXT("append requires 'value'"),
					EMCPErrorCode::InvalidArgument,
					TEXT("`append` action requires a `value` field — a single JSON element matching the array's inner type. To append multiple elements at once, use the `set` action with the full new array."));
			}
			const int32 NewIdx = Helper.AddValue();
			void* ElemPtr = Helper.GetRawPtr(NewIdx);
			if (!FJsonObjectConverter::JsonValueToUProperty(JsonValue, ArrayProp->Inner, ElemPtr, 0, 0))
			{
				Helper.RemoveValues(NewIdx, 1);
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("Failed to convert append value for property '%s'"), *PropertyName),
					EMCPErrorCode::InvalidArgument,
					TEXT("The `value` did not convert through FJsonObjectConverter::JsonValueToUProperty against the array's inner type. Use `read_data_asset` to see the existing array's element shape, then match it — e.g. structs require nested JSON objects with the field names matching the UPROPERTY identifiers."));
			}
			NewArraySize = Helper.Num();
		}
		else if (Action == TEXT("set"))
		{
			TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(TEXT("value"));
			if (!JsonValue.IsValid())
			{
				return FMCPCommonUtils::CreateErrorResponse(
					TEXT("set requires 'value'"),
					EMCPErrorCode::InvalidArgument,
					TEXT("`set` action requires a `value` field in the params object. The value is passed through FJsonObjectConverter::JsonValueToUProperty — for an array property, pass a JSON array; for a non-array, pass the matching JSON primitive/object/struct."));
			}
			// Replace whole array via JsonValueToUProperty on the FArrayProperty.
			if (!FJsonObjectConverter::JsonValueToUProperty(JsonValue, ArrayProp, PropertyData, 0, 0))
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("Failed to convert set value for array property '%s'"), *PropertyName),
					EMCPErrorCode::InvalidArgument,
					TEXT("The `value` did not convert through FJsonObjectConverter::JsonValueToUProperty against the array property. Pass a JSON array of elements matching the array's inner type. Use `read_data_asset` to see the existing array shape as a template."));
			}
			NewArraySize = Helper.Num();
		}
		else
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Unknown action '%s' for array property (use set/append/clear/remove_at)"), *Action),
				EMCPErrorCode::InvalidArgument,
				TEXT("`action` on an array property must be one of: `set` (replace whole array, requires `value`), `append` (push one element, requires `value`), `clear` (empty the array), `remove_at` (drop element at `index`). Case-insensitive."));
		}
	}
	else
	{
		// Non-array: only "set" makes sense.
		if (Action != TEXT("set"))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Action '%s' is array-only — non-array property '%s' supports 'set' only"), *Action, *PropertyName),
				EMCPErrorCode::InvalidArgument,
				TEXT("`append`, `clear`, and `remove_at` are valid only on FArrayProperty targets. The named property is a non-array — use `action: 'set'` with the new value instead. Use `read_data_asset` to see each property's type."));
		}
		TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(TEXT("value"));
		if (!JsonValue.IsValid())
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("set requires 'value'"),
				EMCPErrorCode::InvalidArgument,
				TEXT("`set` action requires a `value` field in the params object. For a non-array property, pass the JSON primitive/object/struct matching the property's CPP type (see `read_data_asset` for the expected shape)."));
		}
		// Whole-struct writes (FLinearColor, FVector, …): a bare JsonValueToUProperty silently produces a
		// ZEROED struct when the bridge delivers the nested value as stringified JSON — it ImportText's the
		// JSON literal against the struct and "succeeds" with garbage/defaults. Route structs through the
		// shared, struct-aware setter (accepts a JSON object OR a JSON/export-text string → JsonObjectToUStruct),
		// the same path actor_set_property uses. Non-struct scalars keep the existing converter (enums-by-name,
		// object/soft refs by path, etc., which SetObjectProperty does not cover).
		if (CastField<FStructProperty>(Property))
		{
			FString StructErr;
			if (!FMCPCommonUtils::SetObjectProperty(Asset, PropertyName, JsonValue, StructErr))
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("Failed to set struct property '%s' (type=%s): %s"), *PropertyName, *Property->GetCPPType(), *StructErr),
					EMCPErrorCode::InvalidArgument,
					TEXT("Struct `value` did not convert. Pass a JSON object whose keys match the struct's UPROPERTY field names (e.g. FLinearColor → {\"R\":4.4,\"G\":4.0,\"B\":2.8,\"A\":1}), or a UE export-text literal string. Use `read_data_asset` to see the expected shape."));
			}
		}
		else if (!FJsonObjectConverter::JsonValueToUProperty(JsonValue, Property, PropertyData, 0, 0))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Failed to convert value for property '%s' (type=%s)"), *PropertyName, *Property->GetCPPType()),
				EMCPErrorCode::InvalidArgument,
				TEXT("`value` did not convert through FJsonObjectConverter::JsonValueToUProperty against the property's CPP type. The error message lists the expected type — match it with the correct JSON shape: structs are JSON objects with field names matching UPROPERTY identifiers, enums are JSON strings of the enum-element name, object refs are JSON strings of the asset path, etc."));
		}
	}

	// ── Persist ────────────────────────────────────────────────────────────
	Asset->Modify();
	Asset->PostEditChange();
	Asset->MarkPackageDirty();
	if (!UEditorAssetLibrary::SaveAsset(AssetPath, /*bOnlyIfIsDirty=*/false))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Property mutated in memory but failed to save to disk: %s"), *AssetPath),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::SaveAsset returned false — the in-memory property change was applied and the package dirtied, but the write to disk failed (source-control checkout failure, read-only file, or a no-op save path). The change is live in the editor for this session; resolve the save blocker and re-save."));
	}

	UE_LOG(LogUnrealMCP, Display,
		TEXT("set_data_asset_property: %s.%s action=%s%s"),
		*AssetPath, *PropertyName, *Action,
		NewArraySize == INDEX_NONE ? TEXT("") : *FString::Printf(TEXT(" → size=%d"), NewArraySize));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("property"), PropertyName);
	Result->SetStringField(TEXT("action"), Action);
	if (NewArraySize != INDEX_NONE)
	{
		Result->SetNumberField(TEXT("array_size"), NewArraySize);
	}
	return Result;
}

// ── read_data_asset ────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPDataAssetCommands::HandleReadDataAsset(
	const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'asset_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`asset_path` is required and must be the full asset path to a UDataAsset, e.g. `/Game/DataAssets/MyAsset`. Use `list_assets` with `asset_type='DataAsset'` to discover existing data assets."));
	}

	UDataAsset* Asset = LoadDataAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Asset not found or not a UDataAsset: %s"), *AssetPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`asset_path` either doesn't exist or resolves to a UObject that is not a UDataAsset. Verify the path with `list_assets` (asset_type='DataAsset'). Paths are case-sensitive and must include `/Game/` prefix."));
	}

	TSharedPtr<FJsonObject> PropertiesJson = MakeShared<FJsonObject>();

	// Walk every edit-exposed FProperty on the class and serialize via the
	// engine's JSON converter.  This handles arrays, structs, soft-refs,
	// enums, etc. transparently — adding a new field on a UDataAsset
	// subclass shows up in the read output for free.
	for (TFieldIterator<FProperty> It(Asset->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;
		if ((Prop->PropertyFlags & (CPF_Edit | CPF_BlueprintVisible)) == 0)
		{
			continue;
		}
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Asset);
		TSharedPtr<FJsonValue> JsonValue = FJsonObjectConverter::UPropertyToJsonValue(Prop, ValuePtr, 0, 0);
		if (JsonValue.IsValid())
		{
			PropertiesJson->SetField(Prop->GetName(), JsonValue);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
	Result->SetObjectField(TEXT("properties"), PropertiesJson);
	return Result;
}
