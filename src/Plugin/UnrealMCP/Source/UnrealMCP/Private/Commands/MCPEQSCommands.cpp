#include "MCPEQSCommands.h"
#include "MCPCommonUtils.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "AssetToolsModule.h"
#include "EditorAssetLibrary.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

// Helper: load EQS query from params
static UEnvQuery* LoadEQSQueryFromParams(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutError)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		OutError = FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'asset_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`asset_path` is required and must be the full asset path to a UEnvQuery, e.g. `/Game/AI/EQS/Q_Patrol`. Use `list_assets` with `asset_type='EnvQuery'` to discover existing queries."));
		return nullptr;
	}

	UEnvQuery* Query = LoadObject<UEnvQuery>(nullptr, *AssetPath);
	if (!Query)
	{
		FString FullPath = AssetPath;
		if (!FullPath.Contains(TEXT(".")))
		{
			FString AssetName = FPaths::GetBaseFilename(FullPath);
			FullPath = FString::Printf(TEXT("%s.%s"), *FullPath, *AssetName);
		}
		Query = LoadObject<UEnvQuery>(nullptr, *FullPath);
	}

	if (!Query)
	{
		OutError = FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to load EQS query: %s"), *AssetPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`asset_path` did not resolve to a UEnvQuery, even with the .AssetName suffix fallback. Verify the path with `list_assets` (asset_type='EnvQuery'). Paths are case-sensitive and must include `/Game/` prefix."));
	}
	return Query;
}

// Helper: serialize all editable properties of a UObject to JSON array
static TArray<TSharedPtr<FJsonValue>> SerializeObjectProperties(UObject* Obj)
{
	TArray<TSharedPtr<FJsonValue>> PropsArray;
	if (!Obj) return PropsArray;

	for (TFieldIterator<FProperty> PropIt(Obj->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

		TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("name"), Prop->GetName());
		PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());

		FString Value;
		const uint8* ValuePtr = Prop->ContainerPtrToValuePtr<uint8>(Obj);
		Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, Obj, PPF_None);
		PropObj->SetStringField(TEXT("value"), Value);

		PropsArray.Add(MakeShared<FJsonValueObject>(PropObj));
	}
	return PropsArray;
}

// ---- HandleCommand ----

TSharedPtr<FJsonObject> FMCPEQSCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("eqs_create"))  return HandleCreateEQSQuery(Params);
	if (CommandType == TEXT("eqs_read"))    return HandleReadEQSQuery(Params);
	if (CommandType == TEXT("eqs_option_add"))    return HandleAddEQSOption(Params);
	if (CommandType == TEXT("eqs_test_add"))      return HandleAddEQSTest(Params);
	if (CommandType == TEXT("eqs_option_remove")) return HandleRemoveEQSOption(Params);
	if (CommandType == TEXT("eqs_test_remove"))   return HandleRemoveEQSTest(Params);
	if (CommandType == TEXT("eqs_set_property"))  return HandleSetEQSProperty(Params);
	if (CommandType == TEXT("eqs_list_types"))    return HandleListEQSTypes(Params);

	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown EQS command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be one of: create_eqs_query, read_eqs_query, add_eqs_option, add_eqs_test, remove_eqs_option, remove_eqs_test, set_eqs_property, list_eqs_types."));
}

// ---- CreateEQSQuery ----

TSharedPtr<FJsonObject> FMCPEQSCommands::HandleCreateEQSQuery(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'asset_path' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`asset_path` is required and must be the full target asset path including the asset name, e.g. `/Game/AI/EQS/Q_Patrol`. The folder will be created if it doesn't exist."));
	}

	FString PackagePath = FPaths::GetPath(AssetPath);
	FString AssetName = FPaths::GetBaseFilename(AssetPath);
	if (AssetName.IsEmpty())
	{
		AssetName = FPaths::GetBaseFilename(PackagePath);
		PackagePath = FPaths::GetPath(PackagePath);
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	// Find EQS query factory
	UFactory* Factory = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->IsChildOf(UFactory::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
		{
			UFactory* CDO = It->GetDefaultObject<UFactory>();
			if (CDO && CDO->SupportedClass == UEnvQuery::StaticClass())
			{
				Factory = NewObject<UFactory>(GetTransientPackage(), *It);
				break;
			}
		}
	}

	if (!Factory)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to find EQS query factory"),
			EMCPErrorCode::Internal,
			TEXT("No registered UFactory advertises UEnvQuery as its SupportedClass. This typically means the AIModule + EnvironmentQueryEditor module isn't loaded — verify both are enabled in the plugin set and that the editor finished initializing. If reproducible, restart the editor."));
	}

	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UEnvQuery::StaticClass(), Factory);
	if (!NewAsset)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to create EQS query at %s/%s"), *PackagePath, *AssetName),
			EMCPErrorCode::Internal,
			TEXT("AssetTools.CreateAsset returned nullptr despite a resolved factory. Common causes: (1) an asset already exists at the path and the factory refused overwrite; (2) destination path is under source control without checkout; (3) the AssetRegistry is mid-scan. Pick a clean destination path or call `delete_asset` first, then retry."));
	}

	// Save
	UPackage* Package = NewAsset->GetOutermost();
	Package->MarkPackageDirty();
	FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	const bool bSaved = UPackage::SavePackage(Package, NewAsset, *PackageFileName, SaveArgs);
	if (!bSaved)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("EQS query created in memory but failed to write package to disk: %s"), *PackageFileName),
			EMCPErrorCode::Internal,
			TEXT("UPackage::SavePackage returned false — the new asset exists in memory (dirty) but is not persisted. Common causes: destination under read-only source control, an invalid path, or a save-validator rejection. Check the editor log; the asset can be saved manually."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
	return Result;
}

// ---- ReadEQSQuery ----

TSharedPtr<FJsonObject> FMCPEQSCommands::HandleReadEQSQuery(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UEnvQuery* Query = LoadEQSQueryFromParams(Params, Error);
	if (!Query) return Error;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), Query->GetPathName());

	TArray<TSharedPtr<FJsonValue>> OptionsArray;
	for (int32 i = 0; i < Query->GetOptions().Num(); ++i)
	{
		UEnvQueryOption* Option = Query->GetOptions()[i];
		if (!Option) continue;
		TSharedPtr<FJsonObject> OptObj = MakeShared<FJsonObject>();
		OptObj->SetNumberField(TEXT("index"), i);

		if (Option->Generator)
		{
			TSharedPtr<FJsonObject> GenObj = MakeShared<FJsonObject>();
			GenObj->SetStringField(TEXT("type"), Option->Generator->GetClass()->GetName());
			GenObj->SetField(TEXT("properties"), MakeShared<FJsonValueArray>(SerializeObjectProperties(Option->Generator)));
			OptObj->SetObjectField(TEXT("generator"), GenObj);
		}

		TArray<TSharedPtr<FJsonValue>> TestsArray;
		for (int32 t = 0; t < Option->Tests.Num(); ++t)
		{
			UEnvQueryTest* Test = Option->Tests[t];
			if (!Test) continue;
			TSharedPtr<FJsonObject> TestObj = MakeShared<FJsonObject>();
			TestObj->SetNumberField(TEXT("index"), t);
			TestObj->SetStringField(TEXT("type"), Test->GetClass()->GetName());
			TestObj->SetField(TEXT("properties"), MakeShared<FJsonValueArray>(SerializeObjectProperties(Test)));
			TestsArray.Add(MakeShared<FJsonValueObject>(TestObj));
		}
		OptObj->SetField(TEXT("tests"), MakeShared<FJsonValueArray>(TestsArray));

		OptionsArray.Add(MakeShared<FJsonValueObject>(OptObj));
	}

	Result->SetField(TEXT("options"), MakeShared<FJsonValueArray>(OptionsArray));
	return Result;
}

// ---- AddEQSOption ----

TSharedPtr<FJsonObject> FMCPEQSCommands::HandleAddEQSOption(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UEnvQuery* Query = LoadEQSQueryFromParams(Params, Error);
	if (!Query) return Error;

	FString GeneratorType;
	if (!Params->TryGetStringField(TEXT("generator_type"), GeneratorType))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'generator_type' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`generator_type` is required (string). Pass the class short-name (e.g. `ActorsOfClass`, `OnCircle`, `PathingGrid`) or the full class name (`EnvQueryGenerator_ActorsOfClass`). Use `list_eqs_types` with `base_class='generator'` to enumerate registered generator classes."));
	}

	// Find generator class
	UClass* GenClass = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->IsChildOf(UEnvQueryGenerator::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
		{
			if (It->GetName() == GeneratorType || It->GetName() == FString::Printf(TEXT("EnvQueryGenerator_%s"), *GeneratorType))
			{
				GenClass = *It;
				break;
			}
		}
	}

	if (!GenClass)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Generator type not found: '%s'. Use list_eqs_types to discover available types."), *GeneratorType),
			EMCPErrorCode::ClassNotLoaded,
			TEXT("`generator_type` must name a non-abstract subclass of UEnvQueryGenerator. Pass either the short name (without the `EnvQueryGenerator_` prefix, e.g. `ActorsOfClass`) or the full class name. Class names are case-sensitive. Use `list_eqs_types` with `base_class='generator'` to discover."));
	}

	// dry_run: every preflight ran (asset load, generator_type lookup) and the
	// property name resolution can run without side effects via FindPropertyByName.
	// Skip the NewObject<>() allocations and the Add into Options. Diff shape per
	// todo/13 phase 5: options_added[] with the would-be index (Add appends to
	// the end so the index is deterministic = current Num()), the resolved
	// generator class name, and a categorization of property names into
	// recognized (would be applied) vs ignored (silently dropped on commit per
	// the existing #DEFERRED partial-success design call). The recognized/
	// ignored split surfaces typos pre-commit without changing apply semantics.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("would_be_index"), Query->GetOptions().Num());
		Entry->SetStringField(TEXT("generator_type"), GenClass->GetName());

		const TSharedPtr<FJsonObject>* DryPropsObj = nullptr;
		if (Params->TryGetObjectField(TEXT("properties"), DryPropsObj))
		{
			TArray<TSharedPtr<FJsonValue>> Recognized;
			TArray<TSharedPtr<FJsonValue>> Ignored;
			for (const auto& KV : (*DryPropsObj)->Values)
			{
				FProperty* Prop = GenClass->FindPropertyByName(FName(*KV.Key));
				if (Prop)
				{
					Recognized.Add(MakeShared<FJsonValueString>(KV.Key));
				}
				else
				{
					Ignored.Add(MakeShared<FJsonValueString>(KV.Key));
				}
			}
			TSharedPtr<FJsonObject> PropsSummary = MakeShared<FJsonObject>();
			PropsSummary->SetArrayField(TEXT("recognized"), Recognized);
			PropsSummary->SetArrayField(TEXT("ignored"), Ignored);
			Entry->SetObjectField(TEXT("properties"), PropsSummary);
		}

		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("options_added"), Arr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	UEnvQueryOption* NewOption = NewObject<UEnvQueryOption>(Query);
	NewOption->Generator = NewObject<UEnvQueryGenerator>(NewOption, GenClass);

	// Edit envelope on the owning UEnvQuery. Brackets the property-override
	// loop and the Add into Query->Options so the transaction captures the
	// complete "add option with these properties" delta as one atomic change,
	// fires OnObjectPropertyChanged, and dirties the asset package. Same
	// pattern as set_eqs_property and the Niagara user-parameter add at
	// MCPNiagaraCommands.cpp:431-440. ImportText_Direct silent-
	// ignore inside the loop is a separate partial-success design call
	// tracked under #DEFERRED — not addressed here.
	Query->PreEditChange(nullptr);

	// Apply property overrides
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj))
	{
		for (const auto& KV : (*PropsObj)->Values)
		{
			FProperty* Prop = GenClass->FindPropertyByName(FName(*KV.Key));
			if (Prop)
			{
				FString TextValue;
				if (KV.Value->Type == EJson::String) TextValue = KV.Value->AsString();
				else if (KV.Value->Type == EJson::Number) TextValue = FString::Printf(TEXT("%g"), KV.Value->AsNumber());
				else if (KV.Value->Type == EJson::Boolean) TextValue = KV.Value->AsBool() ? TEXT("true") : TEXT("false");

				uint8* ValuePtr = Prop->ContainerPtrToValuePtr<uint8>(NewOption->Generator);
				Prop->ImportText_Direct(*TextValue, ValuePtr, NewOption->Generator, PPF_None);
			}
		}
	}

	Query->GetOptionsMutable().Add(NewOption);

	Query->PostEditChange();
	Query->MarkPackageDirty();

	UPackage* Package = Query->GetOutermost();
	FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	if (!UPackage::SavePackage(Package, Query, *PackageFileName, SaveArgs))
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("EQS option added in-memory but failed to persist to disk: %s"), *PackageFileName),
			EMCPErrorCode::Internal,
			TEXT("UPackage::SavePackage returned false — the package was not written. SavePackage no-ops/fails while PIE is active or when the package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the change exists in-memory but will be lost on editor restart."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("option_index"), Query->GetOptions().Num() - 1);
	Result->SetStringField(TEXT("generator_type"), GenClass->GetName());
	return Result;
}

// ---- AddEQSTest ----

TSharedPtr<FJsonObject> FMCPEQSCommands::HandleAddEQSTest(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UEnvQuery* Query = LoadEQSQueryFromParams(Params, Error);
	if (!Query) return Error;

	double OptIdxD;
	if (!Params->TryGetNumberField(TEXT("option_index"), OptIdxD))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'option_index' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`option_index` is required (integer, zero-based). Use `read_eqs_query` to enumerate the query's options — each entry's `index` field is the value to pass."));
	}
	int32 OptIdx = static_cast<int32>(OptIdxD);

	if (OptIdx < 0 || OptIdx >= Query->GetOptions().Num())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("option_index out of range"),
			EMCPErrorCode::OutOfRange,
			TEXT("`option_index` must be in [0, options.length). Use `read_eqs_query` to see the current option count and pick a valid index. Index validation is against the live state — re-read if you've recently added/removed options."));
	}

	FString TestType;
	if (!Params->TryGetStringField(TEXT("test_type"), TestType))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'test_type' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`test_type` is required (string). Pass the class short-name (e.g. `Distance`, `Pathfinding`, `Dot`) or the full class name (`EnvQueryTest_Distance`). Use `list_eqs_types` with `base_class='test'` to enumerate registered test classes."));
	}

	// Find test class
	UClass* TestClass = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->IsChildOf(UEnvQueryTest::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
		{
			if (It->GetName() == TestType || It->GetName() == FString::Printf(TEXT("EnvQueryTest_%s"), *TestType))
			{
				TestClass = *It;
				break;
			}
		}
	}

	if (!TestClass)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Test type not found: '%s'"), *TestType),
			EMCPErrorCode::ClassNotLoaded,
			TEXT("`test_type` must name a non-abstract subclass of UEnvQueryTest. Pass either the short name (without the `EnvQueryTest_` prefix, e.g. `Distance`) or the full class name. Class names are case-sensitive. Use `list_eqs_types` with `base_class='test'` to discover."));
	}

	UEnvQueryOption* Option = Query->GetOptions()[OptIdx];
	if (!Option)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Option at option_index is null (broken EQS asset)"),
			EMCPErrorCode::Internal,
			TEXT("The UEnvQuery's Options array contains a null entry at this index (a broken/partially-loaded template, as the engine's EnvQueryManager also detects). Re-create the option via `add_eqs_option` or repair the asset before adding tests."));

	// dry_run: every preflight ran (asset load, option_index range check,
	// test_type lookup). Property name resolution can run without side effects
	// via FindPropertyByName on TestClass. Skip the NewObject + Tests.Add.
	// Mirrors the AddEQSOption dry-run shape: tests_added[] with the would-be
	// test index (Tests.Add appends so index is deterministic = current Num()),
	// option_index, the resolved test class name, and the recognized/ignored
	// property categorization.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("option_index"), OptIdx);
		Entry->SetNumberField(TEXT("would_be_test_index"), Option->Tests.Num());
		Entry->SetStringField(TEXT("test_type"), TestClass->GetName());

		const TSharedPtr<FJsonObject>* DryPropsObj = nullptr;
		if (Params->TryGetObjectField(TEXT("properties"), DryPropsObj))
		{
			TArray<TSharedPtr<FJsonValue>> Recognized;
			TArray<TSharedPtr<FJsonValue>> Ignored;
			for (const auto& KV : (*DryPropsObj)->Values)
			{
				FProperty* Prop = TestClass->FindPropertyByName(FName(*KV.Key));
				if (Prop)
				{
					Recognized.Add(MakeShared<FJsonValueString>(KV.Key));
				}
				else
				{
					Ignored.Add(MakeShared<FJsonValueString>(KV.Key));
				}
			}
			TSharedPtr<FJsonObject> PropsSummary = MakeShared<FJsonObject>();
			PropsSummary->SetArrayField(TEXT("recognized"), Recognized);
			PropsSummary->SetArrayField(TEXT("ignored"), Ignored);
			Entry->SetObjectField(TEXT("properties"), PropsSummary);
		}

		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("tests_added"), Arr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	UEnvQueryTest* NewTest = NewObject<UEnvQueryTest>(Option, TestClass);

	// Edit envelope on the owning UEnvQuery. Same rationale as HandleAddEQSOption
	// above: brackets the property-override loop and the Tests array mutation so
	// the transaction captures the full delta, fires OnObjectPropertyChanged for
	// the open EQS editor pane, and dirties the asset package. PreEditChange on
	// the Query (outer of Option) is the authoritative MarkPackageDirty target
	// per the #RESEARCH note on EQS sub-object envelope targeting.
	Query->PreEditChange(nullptr);

	// Apply property overrides
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj))
	{
		for (const auto& KV : (*PropsObj)->Values)
		{
			FProperty* Prop = TestClass->FindPropertyByName(FName(*KV.Key));
			if (Prop)
			{
				FString TextValue;
				if (KV.Value->Type == EJson::String) TextValue = KV.Value->AsString();
				else if (KV.Value->Type == EJson::Number) TextValue = FString::Printf(TEXT("%g"), KV.Value->AsNumber());
				else if (KV.Value->Type == EJson::Boolean) TextValue = KV.Value->AsBool() ? TEXT("true") : TEXT("false");

				uint8* ValuePtr = Prop->ContainerPtrToValuePtr<uint8>(NewTest);
				Prop->ImportText_Direct(*TextValue, ValuePtr, NewTest, PPF_None);
			}
		}
	}

	Option->Tests.Add(NewTest);

	Query->PostEditChange();
	Query->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("test_index"), Option->Tests.Num() - 1);
	Result->SetStringField(TEXT("test_type"), TestClass->GetName());
	return Result;
}

// ---- RemoveEQSOption ----

TSharedPtr<FJsonObject> FMCPEQSCommands::HandleRemoveEQSOption(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UEnvQuery* Query = LoadEQSQueryFromParams(Params, Error);
	if (!Query) return Error;

	double OptIdxD;
	if (!Params->TryGetNumberField(TEXT("option_index"), OptIdxD))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'option_index' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`option_index` is required (integer, zero-based). Use `read_eqs_query` to enumerate the query's options — each entry's `index` field is the value to pass."));

	int32 OptIdx = static_cast<int32>(OptIdxD);
	if (OptIdx < 0 || OptIdx >= Query->GetOptions().Num())
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("option_index out of range"),
			EMCPErrorCode::OutOfRange,
			TEXT("`option_index` must be in [0, options.length). Use `read_eqs_query` to see the current option count and pick a valid index. Index validation is against the live state — re-read if you've recently added/removed options."));

	UEnvQueryOption* OptionToRemove = Query->GetOptions()[OptIdx];
	if (!OptionToRemove)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Option at option_index is null (broken EQS asset)"),
			EMCPErrorCode::Internal,
			TEXT("The UEnvQuery's Options array contains a null entry at this index (a broken/partially-loaded template, as the engine's EnvQueryManager also detects). Repair the asset before inspecting or removing this option."));

	// dry_run: every preflight ran (asset load, range check). The cascade is
	// the option's tests — they live on UEnvQueryOption and disappear with
	// their owner. Diff shape: options_removed[] with the index, the existing
	// generator type, and a per-test summary so the caller sees what the
	// cascade reaches.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("option_index"), OptIdx);
		if (OptionToRemove->Generator)
		{
			Entry->SetStringField(TEXT("generator_type"),
				OptionToRemove->Generator->GetClass()->GetName());
		}
		Entry->SetNumberField(TEXT("tests_count"), OptionToRemove->Tests.Num());

		if (OptionToRemove->Tests.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> TestArr;
			for (int32 TIdx = 0; TIdx < OptionToRemove->Tests.Num(); ++TIdx)
			{
				const UEnvQueryTest* T = OptionToRemove->Tests[TIdx];
				TSharedPtr<FJsonObject> TObj = MakeShared<FJsonObject>();
				TObj->SetNumberField(TEXT("test_index"), TIdx);
				TObj->SetStringField(TEXT("test_type"),
					T ? T->GetClass()->GetName() : TEXT("Unknown"));
				TestArr.Add(MakeShared<FJsonValueObject>(TObj));
			}
			Entry->SetArrayField(TEXT("tests_orphaned"), TestArr);
		}

		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("options_removed"), Arr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	// Edit envelope on the owning UEnvQuery. RemoveAt has no failable step so
	// the validate-before-mutate ordering is trivial: range check is above;
	// PreEditChange/PostEditChange/MarkPackageDirty just bracket the mutation.
	Query->PreEditChange(nullptr);
	Query->GetOptionsMutable().RemoveAt(OptIdx);
	Query->PostEditChange();
	Query->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ---- RemoveEQSTest ----

TSharedPtr<FJsonObject> FMCPEQSCommands::HandleRemoveEQSTest(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UEnvQuery* Query = LoadEQSQueryFromParams(Params, Error);
	if (!Query) return Error;

	double OptIdxD, TestIdxD;
	if (!Params->TryGetNumberField(TEXT("option_index"), OptIdxD))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'option_index' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`option_index` is required (integer, zero-based). Use `read_eqs_query` to enumerate the query's options — each entry's `index` field is the value to pass."));
	if (!Params->TryGetNumberField(TEXT("test_index"), TestIdxD))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'test_index' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`test_index` is required (integer, zero-based within the option's Tests array). Use `read_eqs_query` to enumerate the option's tests — each entry's `index` field is the value to pass."));

	int32 OptIdx = static_cast<int32>(OptIdxD);
	int32 TestIdx = static_cast<int32>(TestIdxD);

	if (OptIdx < 0 || OptIdx >= Query->GetOptions().Num())
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("option_index out of range"),
			EMCPErrorCode::OutOfRange,
			TEXT("`option_index` must be in [0, options.length). Use `read_eqs_query` to see the current option count and pick a valid index. Index validation is against the live state — re-read if you've recently added/removed options."));

	UEnvQueryOption* Option = Query->GetOptions()[OptIdx];
	if (!Option)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Option at option_index is null (broken EQS asset)"),
			EMCPErrorCode::Internal,
			TEXT("The UEnvQuery's Options array contains a null entry at this index (a broken/partially-loaded template, as the engine's EnvQueryManager also detects). Repair the asset before removing tests under this option."));
	if (TestIdx < 0 || TestIdx >= Option->Tests.Num())
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("test_index out of range"),
			EMCPErrorCode::OutOfRange,
			TEXT("`test_index` must be in [0, options[option_index].tests.length). Use `read_eqs_query` to see the current test count under the target option and pick a valid index."));

	UEnvQueryTest* TestToRemove = Option->Tests[TestIdx];

	// dry_run: every preflight ran (asset load, both range checks). No cascade
	// — a test is a leaf node in the option/test/generator hierarchy. Diff
	// shape: tests_removed[] mirroring tests_added[]: option_index, test_index,
	// existing test_type.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("option_index"), OptIdx);
		Entry->SetNumberField(TEXT("test_index"), TestIdx);
		Entry->SetStringField(TEXT("test_type"),
			TestToRemove ? TestToRemove->GetClass()->GetName() : TEXT("Unknown"));

		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("tests_removed"), Arr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	// Edit envelope on the owning UEnvQuery. Tests lives on UEnvQueryOption,
	// but the asset-dirty + Details-Panel-refresh target is still Query (the
	// outer package owner) per the established #RESEARCH pattern.
	Query->PreEditChange(nullptr);
	Option->Tests.RemoveAt(TestIdx);
	Query->PostEditChange();
	Query->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ---- SetEQSProperty ----

TSharedPtr<FJsonObject> FMCPEQSCommands::HandleSetEQSProperty(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UEnvQuery* Query = LoadEQSQueryFromParams(Params, Error);
	if (!Query) return Error;

	double OptIdxD;
	if (!Params->TryGetNumberField(TEXT("option_index"), OptIdxD))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'option_index' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`option_index` is required (integer, zero-based). Use `read_eqs_query` to enumerate the query's options — each entry's `index` field is the value to pass."));

	int32 OptIdx = static_cast<int32>(OptIdxD);
	if (OptIdx < 0 || OptIdx >= Query->GetOptions().Num())
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("option_index out of range"),
			EMCPErrorCode::OutOfRange,
			TEXT("`option_index` must be in [0, options.length). Use `read_eqs_query` to see the current option count and pick a valid index. Index validation is against the live state — re-read if you've recently added/removed options."));

	FString Target;
	if (!Params->TryGetStringField(TEXT("target"), Target))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'target' parameter (\"generator\" or test index)"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`target` is required and must be either the literal string `generator` (to set a property on the option's UEnvQueryGenerator) or a numeric test-index string (to set a property on a specific UEnvQueryTest). Use `read_eqs_query` to see the available targets."));

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'property_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`property_name` is required (string, the FProperty name on the target generator or test class). Use `list_eqs_types` to see each class's editable properties, or `read_eqs_query` to see the currently-set properties on an existing target — its `properties[].name` field is the value to pass."));

	TSharedPtr<FJsonValue> PropertyValue = Params->TryGetField(TEXT("property_value"));
	if (!PropertyValue.IsValid())
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'property_value' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`property_value` is required. Accepts JSON string / number / boolean (passed directly to ImportText_Direct), or any JSON object/array (serialized to a string for ImportText). Use the type emitted by `read_eqs_query` for the matching property as a reference."));

	UEnvQueryOption* Option = Query->GetOptions()[OptIdx];
	if (!Option)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Option at option_index is null (broken EQS asset)"),
			EMCPErrorCode::Internal,
			TEXT("The UEnvQuery's Options array contains a null entry at this index (a broken/partially-loaded template, as the engine's EnvQueryManager also detects). Repair the asset before setting properties on this option."));
	UObject* TargetObj = nullptr;

	if (Target == TEXT("generator"))
	{
		TargetObj = Option->Generator;
	}
	else if (Target.IsNumeric())
	{
		int32 TestIdx = FCString::Atoi(*Target);
		if (TestIdx >= 0 && TestIdx < Option->Tests.Num())
		{
			TargetObj = Option->Tests[TestIdx];
		}
	}
	else
	{
		// Without the IsNumeric gate, FCString::Atoi returns 0 for any
		// unparseable string, so a typo'd target ("generatr", "gen", "foo")
		// silently resolved to Tests[0] and the property write landed on the
		// wrong object with success: true.
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Invalid 'target' value '%s' (expected \"generator\" or a numeric test index)"), *Target),
			EMCPErrorCode::InvalidArgument,
			TEXT("`target` must be the literal string `generator` (case-insensitive, addresses the option's UEnvQueryGenerator) or a numeric test-index string such as `\"0\"` (addresses options[option_index].tests[N]). Use `read_eqs_query` to see the option's generator and tests."));
	}

	if (!TargetObj)
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Target object not found"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`target` did not resolve. When `target='generator'`, the option's Generator must be set (use `add_eqs_option` to populate). When `target` is a numeric test index, it must be in [0, options[option_index].tests.length). Use `read_eqs_query` to verify the live state."));

	FProperty* Prop = TargetObj->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Property '%s' not found"), *PropertyName),
			EMCPErrorCode::InvalidArgument,
			TEXT("`property_name` did not match any FProperty on the target's class. Property names are case-sensitive and must match the C++ UPROPERTY identifier (not the editor display name). Use `list_eqs_types` to enumerate the class's editable properties, or `read_eqs_query` to see the property names already in use on a similar target."));

	// JSON → text conversion for ImportText_Direct. Mirrors the conversion in
	// FStateTreeTypeCache::ApplyPropertyOverrides (StateTreeTypeCache.cpp:521-541)
	// so EQS and StateTree property setters accept the same value shapes. The
	// default arm serializes complex JSON back to a string so ImportText sees a
	// non-empty buffer (and can surface a meaningful parse error); without it,
	// any non-primitive value silently passed an empty string to ImportText.
	FString TextValue;
	if (PropertyValue->Type == EJson::String) TextValue = PropertyValue->AsString();
	else if (PropertyValue->Type == EJson::Number) TextValue = FString::Printf(TEXT("%g"), PropertyValue->AsNumber());
	else if (PropertyValue->Type == EJson::Boolean) TextValue = PropertyValue->AsBool() ? TEXT("true") : TEXT("false");
	else
	{
		FString JsonStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
		FJsonSerializer::Serialize(PropertyValue.ToSharedRef(), TEXT(""), Writer);
		TextValue = JsonStr;
	}

	// Edit envelope on the owning UEnvQuery asset. Explicit-close-on-error
	// pattern (per the #RESEARCH note on validate-before-mutate vs
	// explicit-close): ImportText_Direct is the failable engine-side step,
	// so PreEditChange opens the envelope, a null return from ImportText
	// triggers PostEditChange before the error return, and the success path
	// closes with PostEditChange + MarkPackageDirty. Matches the Niagara
	// setter pattern at MCPNiagaraCommands.cpp:431-440.
	uint8* ValuePtr = Prop->ContainerPtrToValuePtr<uint8>(TargetObj);
	Query->PreEditChange(nullptr);
	const TCHAR* ImportResult = Prop->ImportText_Direct(*TextValue, ValuePtr, TargetObj, PPF_None);
	if (!ImportResult)
	{
		Query->PostEditChange();
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to set property '%s' to '%s' on %s"),
				*PropertyName, *TextValue, *TargetObj->GetClass()->GetName()),
			EMCPErrorCode::InvalidArgument,
			TEXT("FProperty::ImportText_Direct returned nullptr for the supplied value text. The value's type or format doesn't match the property's expected ImportText shape. For structs/objects pass the engine T3D-style literal (e.g. `(X=1,Y=2,Z=3)` for FVector); for enums pass the literal name (e.g. `EEnvQueryRunMode::SingleResult`); for object refs pass the asset path. Check the property's CPP type via `list_eqs_types`."));
	}
	Query->PostEditChange();
	Query->MarkPackageDirty();

	// Persist via the file's SavePackage idiom (matches HandleCreateEQSQuery /
	// HandleAddEQSOption) — SavePackage is the -unattended-robust path; the bare
	// UEditorAssetLibrary::SaveAsset no-ops under -unattended (docs/bugs/mcp.md).
	UPackage* Package = Query->GetOutermost();
	FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	if (!UPackage::SavePackage(Package, Query, *PackageFileName, SaveArgs))
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("EQS property '%s' mutated in-memory but failed to persist to disk: %s"), *PropertyName, *PackageFileName),
			EMCPErrorCode::Internal,
			TEXT("UPackage::SavePackage returned false — the package was not written. SavePackage no-ops/fails while PIE is active or when the package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the change exists in-memory but will be lost on editor restart."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ---- ListEQSTypes ----

TSharedPtr<FJsonObject> FMCPEQSCommands::HandleListEQSTypes(const TSharedPtr<FJsonObject>& Params)
{
	FString BaseClass = TEXT("all");
	Params->TryGetStringField(TEXT("base_class"), BaseClass);

	TArray<TSharedPtr<FJsonValue>> TypesArray;

	if (BaseClass == TEXT("all") || BaseClass == TEXT("generator"))
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(UEnvQueryGenerator::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract) && *It != UEnvQueryGenerator::StaticClass())
			{
				TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
				TypeObj->SetStringField(TEXT("class_name"), It->GetName());
				TypeObj->SetStringField(TEXT("category"), TEXT("generator"));
				TypeObj->SetField(TEXT("properties"), MakeShared<FJsonValueArray>(SerializeObjectProperties(It->GetDefaultObject())));
				TypesArray.Add(MakeShared<FJsonValueObject>(TypeObj));
			}
		}
	}

	if (BaseClass == TEXT("all") || BaseClass == TEXT("test"))
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(UEnvQueryTest::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract) && *It != UEnvQueryTest::StaticClass())
			{
				TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
				TypeObj->SetStringField(TEXT("class_name"), It->GetName());
				TypeObj->SetStringField(TEXT("category"), TEXT("test"));
				TypeObj->SetField(TEXT("properties"), MakeShared<FJsonValueArray>(SerializeObjectProperties(It->GetDefaultObject())));
				TypesArray.Add(MakeShared<FJsonValueObject>(TypeObj));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetField(TEXT("types"), MakeShared<FJsonValueArray>(TypesArray));
	Result->SetNumberField(TEXT("count"), TypesArray.Num());
	return Result;
}
