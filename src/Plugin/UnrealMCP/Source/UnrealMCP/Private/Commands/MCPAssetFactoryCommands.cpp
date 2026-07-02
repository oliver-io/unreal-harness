#include "Commands/MCPAssetFactoryCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "Engine/UserDefinedEnum.h"
#include "Engine/UserDefinedStruct.h"
#include "Engine/DataTable.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialFunction.h"
#include "NiagaraScript.h"
#include "NiagaraScriptFactoryNew.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputCoreTypes.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Factories/DataTableFactory.h"
#include "Factories/MaterialParameterCollectionFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Factories/PhysicalMaterialFactoryNew.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "Internationalization/Text.h"

namespace
{
    // Resolve the user-supplied destination into (PackagePath, AssetName).
    // Accepts either:
    //   - separate `path` ("/Game/Foo") + `name` ("MyEnum")
    //   - combined `asset_path` ("/Game/Foo/MyEnum") with no separator field
    bool ResolveDestination(const TSharedPtr<FJsonObject>& Params, FString& OutPackagePath, FString& OutAssetName, FString& OutFullPath, FString& OutError)
    {
        FString Path;
        FString Name;
        Params->TryGetStringField(TEXT("path"), Path);
        Params->TryGetStringField(TEXT("name"), Name);

        if (Path.IsEmpty() || Name.IsEmpty())
        {
            FString Combined;
            if (Params->TryGetStringField(TEXT("asset_path"), Combined) && !Combined.IsEmpty())
            {
                int32 LastSlash = INDEX_NONE;
                if (Combined.FindLastChar(TEXT('/'), LastSlash))
                {
                    Path = Combined.Left(LastSlash);
                    Name = Combined.Mid(LastSlash + 1);
                }
            }
        }

        if (Path.IsEmpty())
        {
            OutError = TEXT("Missing destination 'path' (or full 'asset_path'). Pass /Game/... package path.");
            return false;
        }
        if (Name.IsEmpty())
        {
            OutError = TEXT("Missing 'name' (or include the asset name in 'asset_path').");
            return false;
        }
        if (!Path.StartsWith(TEXT("/")))
        {
            OutError = FString::Printf(TEXT("Path must begin with '/Game/' or '/<Plugin>/' — got: %s"), *Path);
            return false;
        }
        // Asset name validation — reject the obvious illegal characters that UE silently
        // mangles. UE validates more thoroughly in IAssetTools but a fast preflight gives
        // a more useful error message.
        const TCHAR* Invalid = TEXT("/\\:\"*?<>|");
        for (int32 i = 0; Invalid[i] != TEXT('\0'); ++i)
        {
            const TCHAR Ch = Invalid[i];
            if (Name.GetCharArray().Contains(Ch))
            {
                OutError = FString::Printf(TEXT("Asset name contains invalid character '%c': %s"), Ch, *Name);
                return false;
            }
        }

        OutPackagePath = Path;
        OutAssetName = Name;
        OutFullPath = Path / Name;
        return true;
    }

    bool AssetAlreadyExists(const FString& FullPath)
    {
        return UEditorAssetLibrary::DoesAssetExist(FullPath);
    }
}

TSharedPtr<FJsonObject> FMCPAssetFactoryCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("enum_create"))
    {
        return HandleEnumCreate(Params);
    }
    if (CommandType == TEXT("struct_create"))            return HandleStructCreate(Params);
    if (CommandType == TEXT("datatable_create"))         return HandleDatatableCreate(Params);
    if (CommandType == TEXT("mpc_create"))               return HandleMpcCreate(Params);
    if (CommandType == TEXT("material_function_create")) return HandleMaterialFunctionCreate(Params);
    if (CommandType == TEXT("niagara_script_create"))    return HandleNiagaraScriptCreate(Params);
    if (CommandType == TEXT("input_create"))             return HandleInputCreate(Params);
    if (CommandType == TEXT("input_add_mapping"))        return HandleInputAddMapping(Params);
    if (CommandType == TEXT("physics_material_create"))  return HandlePhysicsMaterialCreate(Params);

    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown asset factory command: %s"), *CommandType),
        EMCPErrorCode::InvalidArgument,
        TEXT("Supported asset factory commands in this build: enum_create, struct_create, datatable_create, mpc_create, material_function_create, niagara_script_create, input_create, input_add_mapping, physics_material_create."));
}

TSharedPtr<FJsonObject> FMCPAssetFactoryCommands::HandleEnumCreate(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, AssetName, FullAssetPath, ResolveError;
    if (!ResolveDestination(Params, PackagePath, AssetName, FullAssetPath, ResolveError))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            ResolveError,
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass either {path:'/Game/Foo', name:'MyEnum'} or {asset_path:'/Game/Foo/MyEnum'}."));
    }
    if (AssetAlreadyExists(FullAssetPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s"), *FullAssetPath),
            EMCPErrorCode::NameCollision,
            TEXT("No silent overwrite. Pick a different path or delete the existing asset first."));
    }
    if (!FEnumEditorUtils::IsNameAvailebleForUserDefinedEnum(FName(*AssetName)))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Name unavailable for user-defined enum: %s"), *AssetName),
            EMCPErrorCode::NameCollision,
            TEXT("UE rejected the name — usually because a global UEnum already owns it. Pick a different name."));
    }

    UPackage* Package = CreatePackage(*FullAssetPath);
    if (!Package)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package: %s"), *FullAssetPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UPackage creation failed; the editor may be mid-load or the path may be locked."));
    }

    UEnum* CreatedRaw = FEnumEditorUtils::CreateUserDefinedEnum(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
    UUserDefinedEnum* Created = Cast<UUserDefinedEnum>(CreatedRaw);
    if (!Created)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("FEnumEditorUtils::CreateUserDefinedEnum returned null"),
            EMCPErrorCode::EngineBusy,
            TEXT("UE rejected the creation; check the editor log for the specific reason."));
    }

    // Optional members. Each member entry is either a string (display name) or an object with `display_name`.
    // CreateUserDefinedEnum produces one default member ("NewEnumerator0") + the synthetic _MAX sentinel.
    // We rename that first member to the first user-supplied display name, then append the rest.
    // FEnumEditorUtils makes UDE members sequential (value = index); we don't try to hand-set values.
    TArray<FString> SuppliedDisplayNames;
    const TArray<TSharedPtr<FJsonValue>>* MembersArr;
    if (Params->TryGetArrayField(TEXT("members"), MembersArr))
    {
        for (const TSharedPtr<FJsonValue>& V : *MembersArr)
        {
            if (!V.IsValid()) continue;
            FString DisplayName;
            if (V->Type == EJson::String)
            {
                DisplayName = V->AsString();
            }
            else if (V->Type == EJson::Object)
            {
                V->AsObject()->TryGetStringField(TEXT("display_name"), DisplayName);
            }
            if (!DisplayName.IsEmpty())
            {
                SuppliedDisplayNames.Add(DisplayName);
            }
        }
    }

    for (int32 MemberIdx = 0; MemberIdx < SuppliedDisplayNames.Num(); ++MemberIdx)
    {
        if (MemberIdx > 0)
        {
            // Subsequent members append; UE inserts them just before the _MAX sentinel.
            FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Created);
        }
        FEnumEditorUtils::SetEnumeratorDisplayName(Created, MemberIdx, FText::FromString(SuppliedDisplayNames[MemberIdx]));
    }

    FAssetRegistryModule::AssetCreated(Created);
    Created->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(FullAssetPath, /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset created in memory but SaveAsset wrote nothing to disk: %s"), *FullAssetPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UEditorAssetLibrary::SaveAsset returned false (it no-ops while PIE is active or when the package can't be checked out / saved). Stop PIE and retry; the asset currently exists only in memory."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), FullAssetPath);
    ResultObj->SetStringField(TEXT("class"), TEXT("/Script/Engine.UserDefinedEnum"));
    ResultObj->SetNumberField(TEXT("members_added"), SuppliedDisplayNames.Num());
    {
        TArray<TSharedPtr<FJsonValue>> NamesArr;
        for (const FString& N : SuppliedDisplayNames) NamesArr.Add(MakeShared<FJsonValueString>(N));
        ResultObj->SetArrayField(TEXT("added_member_display_names"), NamesArr);
    }
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPAssetFactoryCommands::HandleStructCreate(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, AssetName, FullAssetPath, ResolveError;
    if (!ResolveDestination(Params, PackagePath, AssetName, FullAssetPath, ResolveError))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            ResolveError,
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass either {path:'/Game/Foo', name:'MyStruct'} or {asset_path:'/Game/Foo/MyStruct'}."));
    }
    if (AssetAlreadyExists(FullAssetPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s"), *FullAssetPath),
            EMCPErrorCode::NameCollision,
            TEXT("No silent overwrite. Pick a different path or delete the existing asset first."));
    }

    UPackage* Package = CreatePackage(*FullAssetPath);
    if (!Package)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package: %s"), *FullAssetPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UPackage creation failed; the editor may be mid-load or the path may be locked."));
    }

    UUserDefinedStruct* Created = FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
    if (!Created)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("FStructureEditorUtils::CreateUserDefinedStruct returned null"),
            EMCPErrorCode::EngineBusy,
            TEXT("UE rejected the creation; check the editor log for the specific reason. The most common cause is a name collision with an existing native FStruct."));
    }

    // Per doc 6: field editing post-create is deferred. Ship the struct shell with whatever
    // CreateUserDefinedStruct seeded by default (UE always adds one default-typed member so the
    // struct is non-empty on disk).

    FAssetRegistryModule::AssetCreated(Created);
    Created->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(FullAssetPath, /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset created in memory but SaveAsset wrote nothing to disk: %s"), *FullAssetPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UEditorAssetLibrary::SaveAsset returned false (it no-ops while PIE is active or when the package can't be checked out / saved). Stop PIE and retry; the asset currently exists only in memory."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), FullAssetPath);
    ResultObj->SetStringField(TEXT("class"), TEXT("/Script/Engine.UserDefinedStruct"));
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

namespace
{
    // Resolve a UScriptStruct from an asset path, fully-qualified script path, or short name.
    // Used by datatable_create to validate the row struct before invoking the factory.
    UScriptStruct* ResolveScriptStruct(const FString& Name)
    {
        if (Name.IsEmpty())
        {
            return nullptr;
        }

        // Asset path → load directly.
        if (Name.StartsWith(TEXT("/")))
        {
            if (UScriptStruct* Loaded = LoadObject<UScriptStruct>(nullptr, *Name))
            {
                return Loaded;
            }
        }

        if (UScriptStruct* Found = FindFirstObject<UScriptStruct>(*Name, EFindFirstObjectOptions::ExactClass))
        {
            return Found;
        }

        // Native struct convention: "FFoo" / "Foo" → try with F prefix stripped/added.
        if (Name.StartsWith(TEXT("F")) && Name.Len() > 1)
        {
            if (UScriptStruct* Found = FindFirstObject<UScriptStruct>(*Name.RightChop(1), EFindFirstObjectOptions::ExactClass))
            {
                return Found;
            }
        }
        else if (!Name.StartsWith(TEXT("F")))
        {
            const FString WithF = FString::Printf(TEXT("F%s"), *Name);
            if (UScriptStruct* Found = FindFirstObject<UScriptStruct>(*WithF, EFindFirstObjectOptions::ExactClass))
            {
                return Found;
            }
        }

        return nullptr;
    }
}

TSharedPtr<FJsonObject> FMCPAssetFactoryCommands::HandleDatatableCreate(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, AssetName, FullAssetPath, ResolveError;
    if (!ResolveDestination(Params, PackagePath, AssetName, FullAssetPath, ResolveError))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            ResolveError,
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass {path:'/Game/Data/Tables', name:'DT_Foo'} or {asset_path:'/Game/Data/Tables/DT_Foo'}."));
    }
    if (AssetAlreadyExists(FullAssetPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s"), *FullAssetPath),
            EMCPErrorCode::NameCollision,
            TEXT("No silent overwrite. Pick a different path or delete the existing asset first."));
    }

    // Per doc 6 invariant: the row struct path must resolve to a UScriptStruct
    // before the factory runs. Empty/invalid is a structured error.
    FString RowStructPath;
    if (!Params->TryGetStringField(TEXT("row_struct"), RowStructPath) || RowStructPath.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'row_struct' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("DataTables require a row struct. Pass an asset path (UserDefinedStruct), script path, or short name (e.g. 'FMyRowStruct' or '/Script/MyGame.MyRowStruct')."));
    }

    UScriptStruct* RowStruct = ResolveScriptStruct(RowStructPath);
    if (!RowStruct)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve row struct: %s"), *RowStructPath),
            EMCPErrorCode::ClassNotLoaded,
            TEXT("Verify spelling. UserDefinedStructs live under /Game/...; native structs under /Script/<Module>. Use struct_create first if it doesn't exist."));
    }

    UDataTableFactory* Factory = NewObject<UDataTableFactory>();
    Factory->Struct = RowStruct;

    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
    UObject* CreatedObj = AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, UDataTable::StaticClass(), Factory);
    UDataTable* Created = Cast<UDataTable>(CreatedObj);
    if (!Created)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("UAssetTools::CreateAsset returned null for the DataTable"),
            EMCPErrorCode::EngineBusy,
            TEXT("UE rejected the creation; check the editor log."));
    }

    Created->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(FullAssetPath, /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset created in memory but SaveAsset wrote nothing to disk: %s"), *FullAssetPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UEditorAssetLibrary::SaveAsset returned false (it no-ops while PIE is active or when the package can't be checked out / saved). Stop PIE and retry; the asset currently exists only in memory."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), FullAssetPath);
    ResultObj->SetStringField(TEXT("class"), TEXT("/Script/Engine.DataTable"));
    ResultObj->SetStringField(TEXT("row_struct"), RowStruct->GetPathName());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPAssetFactoryCommands::HandleMpcCreate(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, AssetName, FullAssetPath, ResolveError;
    if (!ResolveDestination(Params, PackagePath, AssetName, FullAssetPath, ResolveError))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            ResolveError,
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass {path:'/Game/Materials/Collections', name:'MPC_Foo'} or {asset_path:'/Game/Materials/Collections/MPC_Foo'}."));
    }
    if (AssetAlreadyExists(FullAssetPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s"), *FullAssetPath),
            EMCPErrorCode::NameCollision,
            TEXT("No silent overwrite. Pick a different path or delete the existing asset first."));
    }

    UMaterialParameterCollectionFactoryNew* Factory = NewObject<UMaterialParameterCollectionFactoryNew>();

    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
    UObject* CreatedObj = AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, UMaterialParameterCollection::StaticClass(), Factory);
    UMaterialParameterCollection* Created = Cast<UMaterialParameterCollection>(CreatedObj);
    if (!Created)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("UAssetTools::CreateAsset returned null for the MaterialParameterCollection"),
            EMCPErrorCode::EngineBusy,
            TEXT("UE rejected the creation; check the editor log."));
    }

    // Optional initial parameter list. Per doc 6: "Initial scalar / vector parameter list (optional)".
    // Each entry is an object: {type: "scalar"|"vector", name, default_value (number for scalar,
    // {r,g,b,a} for vector)}. Unknown / malformed entries are skipped silently — the doc invariant
    // is "no graph editing here", so we accept simple seeding but don't validate against a deeper
    // parameter-language.
    int32 ScalarsAdded = 0;
    int32 VectorsAdded = 0;
    const TArray<TSharedPtr<FJsonValue>>* ParametersArr;
    if (Params->TryGetArrayField(TEXT("parameters"), ParametersArr))
    {
        for (const TSharedPtr<FJsonValue>& V : *ParametersArr)
        {
            if (!V.IsValid() || V->Type != EJson::Object) continue;
            const TSharedPtr<FJsonObject> Obj = V->AsObject();
            FString Type, Name;
            Obj->TryGetStringField(TEXT("type"), Type);
            Obj->TryGetStringField(TEXT("name"), Name);
            if (Name.IsEmpty()) continue;

            if (Type.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
            {
                FCollectionScalarParameter Entry;
                Entry.ParameterName = FName(*Name);
                double DefaultValue = 0.0;
                Obj->TryGetNumberField(TEXT("default_value"), DefaultValue);
                Entry.DefaultValue = static_cast<float>(DefaultValue);
                Created->ScalarParameters.Add(Entry);
                ++ScalarsAdded;
            }
            else if (Type.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
            {
                FCollectionVectorParameter Entry;
                Entry.ParameterName = FName(*Name);
                const TSharedPtr<FJsonObject>* DefaultObj;
                if (Obj->TryGetObjectField(TEXT("default_value"), DefaultObj) && DefaultObj->IsValid())
                {
                    Entry.DefaultValue.R = static_cast<float>((*DefaultObj)->GetNumberField(TEXT("r")));
                    Entry.DefaultValue.G = static_cast<float>((*DefaultObj)->GetNumberField(TEXT("g")));
                    Entry.DefaultValue.B = static_cast<float>((*DefaultObj)->GetNumberField(TEXT("b")));
                    Entry.DefaultValue.A = static_cast<float>((*DefaultObj)->GetNumberField(TEXT("a")));
                }
                Created->VectorParameters.Add(Entry);
                ++VectorsAdded;
            }
        }
    }

    Created->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(FullAssetPath, /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset created in memory but SaveAsset wrote nothing to disk: %s"), *FullAssetPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UEditorAssetLibrary::SaveAsset returned false (it no-ops while PIE is active or when the package can't be checked out / saved). Stop PIE and retry; the asset currently exists only in memory."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), FullAssetPath);
    ResultObj->SetStringField(TEXT("class"), TEXT("/Script/Engine.MaterialParameterCollection"));
    ResultObj->SetNumberField(TEXT("scalars_added"), ScalarsAdded);
    ResultObj->SetNumberField(TEXT("vectors_added"), VectorsAdded);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPAssetFactoryCommands::HandleMaterialFunctionCreate(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, AssetName, FullAssetPath, ResolveError;
    if (!ResolveDestination(Params, PackagePath, AssetName, FullAssetPath, ResolveError))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            ResolveError,
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass {path:'/Game/Materials/Functions', name:'MF_Foo'} or {asset_path:'/Game/Materials/Functions/MF_Foo'}."));
    }
    if (AssetAlreadyExists(FullAssetPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s"), *FullAssetPath),
            EMCPErrorCode::NameCollision,
            TEXT("No silent overwrite. Pick a different path or delete the existing asset first."));
    }

    UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();

    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
    UObject* CreatedObj = AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, UMaterialFunction::StaticClass(), Factory);
    UMaterialFunction* Created = Cast<UMaterialFunction>(CreatedObj);
    if (!Created)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("UAssetTools::CreateAsset returned null for the MaterialFunction"),
            EMCPErrorCode::EngineBusy,
            TEXT("UE rejected the creation; check the editor log."));
    }

    // Optional description / category metadata. Per doc 6 invariant ("no graph
    // editing here"), expressions are added via the existing add_material_expression
    // tool — this surface ships an empty graph and lets the caller seed only the
    // small set of human-facing strings UE surfaces in the material function picker.
    FString Description;
    if (Params->TryGetStringField(TEXT("description"), Description) && !Description.IsEmpty())
    {
        Created->Description = Description;
    }
    bool bExposeToLibrary = false;
    if (Params->TryGetBoolField(TEXT("expose_to_library"), bExposeToLibrary))
    {
        Created->bExposeToLibrary = bExposeToLibrary;
    }

    Created->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(FullAssetPath, /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset created in memory but SaveAsset wrote nothing to disk: %s"), *FullAssetPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UEditorAssetLibrary::SaveAsset returned false (it no-ops while PIE is active or when the package can't be checked out / saved). Stop PIE and retry; the asset currently exists only in memory."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), FullAssetPath);
    ResultObj->SetStringField(TEXT("class"), TEXT("/Script/Engine.MaterialFunction"));
    if (!Description.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("description"), Description);
    }
    ResultObj->SetBoolField(TEXT("expose_to_library"), Created->bExposeToLibrary);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPAssetFactoryCommands::HandleNiagaraScriptCreate(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, AssetName, FullAssetPath, ResolveError;
    if (!ResolveDestination(Params, PackagePath, AssetName, FullAssetPath, ResolveError))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            ResolveError,
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass {path:'/Game/FX/NiagaraScripts', name:'NS_Foo'} or {asset_path:'/Game/FX/NiagaraScripts/NS_Foo'}."));
    }
    if (AssetAlreadyExists(FullAssetPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s"), *FullAssetPath),
            EMCPErrorCode::NameCollision,
            TEXT("No silent overwrite. Pick a different path or delete the existing asset first."));
    }

    // Per doc 6: usage selects between Module / Function / DynamicInput.
    // Each maps to a concrete UNiagaraScriptFactoryNew subclass.
    FString UsageStr;
    if (!Params->TryGetStringField(TEXT("usage"), UsageStr) || UsageStr.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'usage' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Closed set: 'module' (UNiagaraModuleScriptFactory), 'function' (UNiagaraFunctionScriptFactory), 'dynamic_input' (UNiagaraDynamicInputScriptFactory)."));
    }

    UClass* FactoryClass = nullptr;
    const TCHAR* WireUsage = nullptr;
    const FString UsageLower = UsageStr.ToLower();
    if (UsageLower == TEXT("module"))
    {
        FactoryClass = UNiagaraModuleScriptFactory::StaticClass();
        WireUsage = TEXT("module");
    }
    else if (UsageLower == TEXT("function"))
    {
        FactoryClass = UNiagaraFunctionScriptFactory::StaticClass();
        WireUsage = TEXT("function");
    }
    else if (UsageLower == TEXT("dynamic_input") || UsageLower == TEXT("dynamicinput"))
    {
        FactoryClass = UNiagaraDynamicInputScriptFactory::StaticClass();
        WireUsage = TEXT("dynamic_input");
    }
    else
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown 'usage' value: %s"), *UsageStr),
            EMCPErrorCode::InvalidArgument,
            TEXT("Closed set: 'module', 'function', 'dynamic_input' (case-insensitive)."));
    }

    UNiagaraScriptFactoryNew* Factory = NewObject<UNiagaraScriptFactoryNew>(GetTransientPackage(), FactoryClass);
    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
    UObject* CreatedObj = AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, UNiagaraScript::StaticClass(), Factory);
    UNiagaraScript* Created = Cast<UNiagaraScript>(CreatedObj);
    if (!Created)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("UAssetTools::CreateAsset returned null for the NiagaraScript"),
            EMCPErrorCode::EngineBusy,
            TEXT("UE rejected the creation; check the editor log."));
    }

    Created->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(FullAssetPath, /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset created in memory but SaveAsset wrote nothing to disk: %s"), *FullAssetPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UEditorAssetLibrary::SaveAsset returned false (it no-ops while PIE is active or when the package can't be checked out / saved). Stop PIE and retry; the asset currently exists only in memory."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), FullAssetPath);
    ResultObj->SetStringField(TEXT("class"), TEXT("/Script/Niagara.NiagaraScript"));
    ResultObj->SetStringField(TEXT("usage"), WireUsage);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPAssetFactoryCommands::HandleInputCreate(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, AssetName, FullAssetPath, ResolveError;
    if (!ResolveDestination(Params, PackagePath, AssetName, FullAssetPath, ResolveError))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            ResolveError,
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass {path:'/Game/Input', name:'IA_Foo'} or {asset_path:'/Game/Input/IA_Foo'}."));
    }
    if (AssetAlreadyExists(FullAssetPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s"), *FullAssetPath),
            EMCPErrorCode::NameCollision,
            TEXT("No silent overwrite. Pick a different path or delete the existing asset first."));
    }

    // Per doc 6: type discriminates between Action and MappingContext. The two
    // assets are unrelated UClasses but share enough surface that a 2-way
    // input_create(type=...) stays predictable per philosophy #2 (variadic
    // over multiplicative).
    FString TypeStr;
    if (!Params->TryGetStringField(TEXT("type"), TypeStr) || TypeStr.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'type' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Closed set: 'action' (UInputAction) or 'mapping_context' (UInputMappingContext)."));
    }
    const FString TypeLower = TypeStr.ToLower();

    UPackage* Package = CreatePackage(*FullAssetPath);
    if (!Package)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create package: %s"), *FullAssetPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UPackage creation failed; the editor may be mid-load or the path may be locked."));
    }

    UObject* Created = nullptr;
    FString ClassPath;
    FString WireValueType;

    if (TypeLower == TEXT("action"))
    {
        UInputAction* Action = NewObject<UInputAction>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
        if (!Action)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("NewObject<UInputAction> returned null"),
                EMCPErrorCode::EngineBusy,
                TEXT("UE rejected the construction; check the editor log."));
        }

        // Optional value_type — closed set per EInputActionValueType.
        FString ValueTypeStr;
        EInputActionValueType ValueType = EInputActionValueType::Boolean;
        WireValueType = TEXT("boolean");
        if (Params->TryGetStringField(TEXT("value_type"), ValueTypeStr) && !ValueTypeStr.IsEmpty())
        {
            const FString VTL = ValueTypeStr.ToLower();
            if      (VTL == TEXT("boolean") || VTL == TEXT("bool") || VTL == TEXT("digital")) { ValueType = EInputActionValueType::Boolean; WireValueType = TEXT("boolean"); }
            else if (VTL == TEXT("axis1d")  || VTL == TEXT("float"))                          { ValueType = EInputActionValueType::Axis1D;  WireValueType = TEXT("axis1d"); }
            else if (VTL == TEXT("axis2d")  || VTL == TEXT("vector2d") || VTL == TEXT("vec2")) { ValueType = EInputActionValueType::Axis2D;  WireValueType = TEXT("axis2d"); }
            else if (VTL == TEXT("axis3d")  || VTL == TEXT("vector")   || VTL == TEXT("vec3")) { ValueType = EInputActionValueType::Axis3D;  WireValueType = TEXT("axis3d"); }
            else
            {
                return FMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Unknown 'value_type' value: %s"), *ValueTypeStr),
                    EMCPErrorCode::InvalidArgument,
                    TEXT("Closed set: 'boolean' (Digital), 'axis1d' (float), 'axis2d' (Vector2D), 'axis3d' (Vector). Aliases accepted."));
            }
        }
        Action->ValueType = ValueType;
        Created = Action;
        ClassPath = TEXT("/Script/EnhancedInput.InputAction");
    }
    else if (TypeLower == TEXT("mapping_context") || TypeLower == TEXT("mappingcontext") || TypeLower == TEXT("imc"))
    {
        UInputMappingContext* IMC = NewObject<UInputMappingContext>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
        if (!IMC)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("NewObject<UInputMappingContext> returned null"),
                EMCPErrorCode::EngineBusy,
                TEXT("UE rejected the construction; check the editor log."));
        }
        Created = IMC;
        ClassPath = TEXT("/Script/EnhancedInput.InputMappingContext");
    }
    else
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown 'type' value: %s"), *TypeStr),
            EMCPErrorCode::InvalidArgument,
            TEXT("Closed set: 'action' (UInputAction), 'mapping_context' (UInputMappingContext). Case-insensitive."));
    }

    FAssetRegistryModule::AssetCreated(Created);
    Created->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(FullAssetPath, /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset created in memory but SaveAsset wrote nothing to disk: %s"), *FullAssetPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UEditorAssetLibrary::SaveAsset returned false (it no-ops while PIE is active or when the package can't be checked out / saved). Stop PIE and retry; the asset currently exists only in memory."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), FullAssetPath);
    ResultObj->SetStringField(TEXT("class"), ClassPath);
    ResultObj->SetStringField(TEXT("type"), (TypeLower == TEXT("action")) ? TEXT("action") : TEXT("mapping_context"));
    if (TypeLower == TEXT("action"))
    {
        ResultObj->SetStringField(TEXT("value_type"), WireValueType);
    }
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// GAP-002: add one or more key→action mapping rows to an existing IMC.
// input_create makes the UInputAction / UInputMappingContext shells but nothing
// wires a key to an action. UInputMappingContext::MapKey(Action, FKey) (the public
// BlueprintCallable accessor, EnhancedInput InputMappingContext.h:224-225) appends a
// FEnhancedActionKeyMapping to DefaultKeyMappings.Mappings — exactly what the IMC
// editor's "+" button does. FKey is built from the EKeys name string
// (InputCoreTypes.h:62 FKey(const TCHAR*)) and validated via FKey::IsValid()
// (InputCoreTypes.cpp:1294 — false for unregistered names) before the mutation.
TSharedPtr<FJsonObject> FMCPAssetFactoryCommands::HandleInputAddMapping(const TSharedPtr<FJsonObject>& Params)
{
    // The IMC to mutate. Accept context_path / asset_path / path aliases.
    FString ContextPath;
    if (!Params->TryGetStringField(TEXT("context_path"), ContextPath) || ContextPath.IsEmpty())
    {
        Params->TryGetStringField(TEXT("asset_path"), ContextPath);
    }
    if (ContextPath.IsEmpty())
    {
        Params->TryGetStringField(TEXT("path"), ContextPath);
    }
    if (ContextPath.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'context_path' (the UInputMappingContext asset to mutate)"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass {context_path:'/Game/Input/IMC_Foo', action_path:'/Game/Input/IA_Foo', key:'W'}."));
    }

    FString ActionPath;
    if (!Params->TryGetStringField(TEXT("action_path"), ActionPath) || ActionPath.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'action_path' (the UInputAction to bind)"),
            EMCPErrorCode::InvalidArgument,
            TEXT("action_path must point at a UInputAction asset, e.g. '/Game/Input/IA_Jump'."));
    }

    // Collect the key(s): single `key` string and/or a `keys` array. At least one required.
    TArray<FString> KeyStrings;
    {
        FString SingleKey;
        if (Params->TryGetStringField(TEXT("key"), SingleKey) && !SingleKey.IsEmpty())
        {
            KeyStrings.Add(SingleKey);
        }
        const TArray<TSharedPtr<FJsonValue>>* KeysArr = nullptr;
        if (Params->TryGetArrayField(TEXT("keys"), KeysArr))
        {
            for (const TSharedPtr<FJsonValue>& V : *KeysArr)
            {
                if (V.IsValid() && V->Type == EJson::String && !V->AsString().IsEmpty())
                {
                    KeyStrings.Add(V->AsString());
                }
            }
        }
    }
    if (KeyStrings.Num() == 0)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing key(s): provide 'key' (string) and/or 'keys' (string array)"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Use an EKeys name, e.g. 'W', 'SpaceBar', 'LeftMouseButton', 'Gamepad_FaceButton_Bottom'."));
    }

    // Load the IMC.
    UObject* ContextObj = UEditorAssetLibrary::LoadAsset(ContextPath);
    UInputMappingContext* IMC = Cast<UInputMappingContext>(ContextObj);
    if (!IMC)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("context_path did not resolve to a UInputMappingContext: %s"), *ContextPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("Create it first with input_create {type:'mapping_context'}; pass the IMC's asset path."));
    }

    // Load the action.
    UObject* ActionObj = UEditorAssetLibrary::LoadAsset(ActionPath);
    UInputAction* Action = Cast<UInputAction>(ActionObj);
    if (!Action)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("action_path did not resolve to a UInputAction: %s"), *ActionPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("Create it first with input_create {type:'action'}; pass the IA's asset path."));
    }

    // Validate every key BEFORE mutating so a bad key in a batch is all-or-nothing.
    for (const FString& KeyStr : KeyStrings)
    {
        const FKey Key(*KeyStr);
        if (!Key.IsValid())
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unknown / invalid key name: '%s'"), *KeyStr),
                EMCPErrorCode::InvalidArgument,
                TEXT("Key must be a registered EKeys name (case-sensitive), e.g. 'W', 'SpaceBar', 'LeftMouseButton', 'Gamepad_LeftStick_Up'."));
        }
    }

    // Mutate: editor-transactional, then dirty + auto-save (sibling-handler house style).
    IMC->Modify();
    TArray<TSharedPtr<FJsonValue>> AddedKeys;
    for (const FString& KeyStr : KeyStrings)
    {
        IMC->MapKey(Action, FKey(*KeyStr));
        AddedKeys.Add(MakeShared<FJsonValueString>(KeyStr));
    }
    IMC->PostEditChange();
    IMC->MarkPackageDirty();

    const FString FullAssetPath = IMC->GetPathName();
    if (!UEditorAssetLibrary::SaveLoadedAsset(IMC, /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Mappings added in memory but SaveLoadedAsset wrote nothing to disk: %s"), *ContextPath),
            EMCPErrorCode::EngineBusy,
            TEXT("Save no-ops while PIE is active or when the package can't be checked out / saved. Stop PIE and retry; the change currently exists only in memory."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), IMC->GetPackage() ? IMC->GetPackage()->GetName() : ContextPath);
    ResultObj->SetStringField(TEXT("context_path"), FullAssetPath);
    ResultObj->SetStringField(TEXT("action_path"), Action->GetPathName());
    ResultObj->SetNumberField(TEXT("keys_added"), AddedKeys.Num());
    ResultObj->SetArrayField(TEXT("keys"), AddedKeys);
    // Total rows now on the IMC's default mappings (the live array MapKey appends to).
    ResultObj->SetNumberField(TEXT("mappings_count"), IMC->GetMappings().Num());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// UPhysicalMaterial shell. A per-material combine mode only takes effect when its
// bOverride*CombineMode flag is set (otherwise the Project Settings > Physics default
// wins), so passing friction_combine_mode / restitution_combine_mode flips the
// matching override flag as one unit.
TSharedPtr<FJsonObject> FMCPAssetFactoryCommands::HandlePhysicsMaterialCreate(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, AssetName, FullAssetPath, ResolveError;
    if (!ResolveDestination(Params, PackagePath, AssetName, FullAssetPath, ResolveError))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            ResolveError,
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass {path:'/Game/Physics', name:'PM_Bouncy'} or {asset_path:'/Game/Physics/PM_Bouncy'}."));
    }
    if (AssetAlreadyExists(FullAssetPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s"), *FullAssetPath),
            EMCPErrorCode::NameCollision,
            TEXT("No silent overwrite. Pick a different path or delete the existing asset first."));
    }

    // Validate combine-mode strings BEFORE the factory runs (doc-6 invariant).
    auto ParseCombineMode = [](const FString& Text, TEnumAsByte<EFrictionCombineMode::Type>& Out) -> bool
    {
        if (Text.Equals(TEXT("Average"),  ESearchCase::IgnoreCase)) { Out = EFrictionCombineMode::Average;  return true; }
        if (Text.Equals(TEXT("Min"),      ESearchCase::IgnoreCase)) { Out = EFrictionCombineMode::Min;      return true; }
        if (Text.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase)) { Out = EFrictionCombineMode::Multiply; return true; }
        if (Text.Equals(TEXT("Max"),      ESearchCase::IgnoreCase)) { Out = EFrictionCombineMode::Max;      return true; }
        return false;
    };

    FString FrictionModeStr, RestitutionModeStr;
    TEnumAsByte<EFrictionCombineMode::Type> FrictionMode = EFrictionCombineMode::Average;
    TEnumAsByte<EFrictionCombineMode::Type> RestitutionMode = EFrictionCombineMode::Average;
    const bool bHasFrictionMode = Params->TryGetStringField(TEXT("friction_combine_mode"), FrictionModeStr) && !FrictionModeStr.IsEmpty();
    const bool bHasRestitutionMode = Params->TryGetStringField(TEXT("restitution_combine_mode"), RestitutionModeStr) && !RestitutionModeStr.IsEmpty();
    for (const FString* ModeStr : { bHasFrictionMode ? &FrictionModeStr : nullptr,
                                    bHasRestitutionMode ? &RestitutionModeStr : nullptr })
    {
        TEnumAsByte<EFrictionCombineMode::Type> Ignored;
        if (ModeStr && !ParseCombineMode(*ModeStr, Ignored))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Invalid combine mode '%s'"), **ModeStr),
                EMCPErrorCode::InvalidArgument,
                TEXT("Combine modes are 'Average', 'Min', 'Multiply', or 'Max' (case-insensitive)."));
        }
    }
    if (bHasFrictionMode) ParseCombineMode(FrictionModeStr, FrictionMode);
    if (bHasRestitutionMode) ParseCombineMode(RestitutionModeStr, RestitutionMode);

    double Restitution = 0.0;
    if (Params->TryGetNumberField(TEXT("restitution"), Restitution) && (Restitution < 0.0 || Restitution > 1.0))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("restitution %f out of range"), Restitution),
            EMCPErrorCode::InvalidArgument,
            TEXT("Restitution (bounciness) must be in [0, 1]."));
    }

    UPhysicalMaterialFactoryNew* Factory = NewObject<UPhysicalMaterialFactoryNew>();

    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
    UObject* CreatedObj = AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, UPhysicalMaterial::StaticClass(), Factory);
    UPhysicalMaterial* Created = Cast<UPhysicalMaterial>(CreatedObj);
    if (!Created)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("UAssetTools::CreateAsset returned null for the PhysicalMaterial"),
            EMCPErrorCode::EngineBusy,
            TEXT("UE rejected the creation; check the editor log."));
    }

    double Value = 0.0;
    if (Params->TryGetNumberField(TEXT("friction"), Value))         Created->Friction = static_cast<float>(Value);
    if (Params->TryGetNumberField(TEXT("static_friction"), Value))  Created->StaticFriction = static_cast<float>(Value);
    if (Params->TryGetNumberField(TEXT("restitution"), Value))      Created->Restitution = static_cast<float>(Value);
    if (Params->TryGetNumberField(TEXT("density"), Value))          Created->Density = static_cast<float>(Value);
    if (bHasFrictionMode)
    {
        Created->FrictionCombineMode = FrictionMode;
        Created->bOverrideFrictionCombineMode = true;
    }
    if (bHasRestitutionMode)
    {
        Created->RestitutionCombineMode = RestitutionMode;
        Created->bOverrideRestitutionCombineMode = true;
    }

    Created->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(FullAssetPath, /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset created in memory but SaveAsset wrote nothing to disk: %s"), *FullAssetPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UEditorAssetLibrary::SaveAsset returned false (it no-ops while PIE is active or when the package can't be checked out / saved). Stop PIE and retry; the asset currently exists only in memory."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), FullAssetPath);
    ResultObj->SetStringField(TEXT("class"), TEXT("/Script/PhysicsCore.PhysicalMaterial"));
    ResultObj->SetNumberField(TEXT("friction"), Created->Friction);
    ResultObj->SetNumberField(TEXT("static_friction"), Created->StaticFriction);
    ResultObj->SetNumberField(TEXT("restitution"), Created->Restitution);
    ResultObj->SetNumberField(TEXT("density"), Created->Density);
    ResultObj->SetBoolField(TEXT("friction_combine_override"), Created->bOverrideFrictionCombineMode);
    ResultObj->SetBoolField(TEXT("restitution_combine_override"), Created->bOverrideRestitutionCombineMode);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}
