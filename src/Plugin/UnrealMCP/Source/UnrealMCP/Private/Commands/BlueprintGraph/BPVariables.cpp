#include "Commands/BlueprintGraph/BPVariables.h"
#include "Commands/MCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "EditorSubsystem.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "PropertyEditorModule.h"
#include "Modules/ModuleManager.h"

TSharedPtr<FJsonObject> FBPVariables::CreateVariable(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    FString BlueprintName = Params->GetStringField(TEXT("blueprint_name"));
    FString VariableName = Params->GetStringField(TEXT("variable_name"));
    FString VariableType = Params->GetStringField(TEXT("variable_type"));

    bool IsPublic = Params->HasField(TEXT("is_public")) ? Params->GetBoolField(TEXT("is_public")) : false;
    FString Tooltip = Params->HasField(TEXT("tooltip")) ? Params->GetStringField(TEXT("tooltip")) : TEXT("");
    FString Category = Params->HasField(TEXT("category")) ? Params->GetStringField(TEXT("category")) : TEXT("Default");

    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);

    if (!Blueprint)
    {
        Result->SetBoolField("success", false);
        Result->SetStringField("error", "Blueprint not found");
        return Result;
    }

    FEdGraphPinType VarType = GetPinTypeFromString(VariableType);
    if (VariableName.Len() >= NAME_SIZE)
    {
        Result->SetBoolField("success", false);
        Result->SetStringField("error", FString::Printf(TEXT("variable_name is %d characters; FName names are limited to %d"), VariableName.Len(), NAME_SIZE - 1));
        return Result;
    }
    FName VarName = FName(*VariableName);

    // dry_run: every preflight ran (field reads, FindBlueprint, GetPinTypeFromString).
    // The remaining work — AddMemberVariable + property flag setup + MarkPackageDirty
    // + MarkBlueprintAsModified + CompileBlueprint — is the side effect we skip. Diff
    // shape per todo/13 phase 2: variables_added[]. The would-be index in
    // Blueprint->NewVariables is deterministic (AddMemberVariable appends → would_be
    // = current Num()), so we emit it directly. Same convention as add_skeleton_socket
    // (Phase 4c) and EQS deterministic-index variant (Phase 5e).
    //
    // We additionally do a defensive duplicate-name check by walking NewVariables.
    // Apply rejects duplicates implicitly via AddMemberVariable returning false (with
    // a generic "Failed to create variable" error); dry-run surfaces the specific
    // duplicate cause up-front so callers see WHY a hypothetical apply would fail.
    // Same outcome (duplicate rejected) — better diagnostic.
    if (FMCPCommonUtils::ParseDryRun(Params))
    {
        for (const FBPVariableDescription& Var : Blueprint->NewVariables)
        {
            if (Var.VarName == VarName)
            {
                return FMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Variable '%s' already exists on this blueprint"), *VariableName),
                    EMCPErrorCode::NameCollision,
                    TEXT("Pick a different `variable_name`, or use `bp_set_variable_properties` to modify the existing variable. `bp_get_variable_details` lists current variables."));
            }
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
        Entry->SetStringField(TEXT("variable_name"), VariableName);
        Entry->SetStringField(TEXT("variable_type"), VariableType);
        Entry->SetBoolField(TEXT("is_public"), IsPublic);
        Entry->SetStringField(TEXT("category"), Category);
        Entry->SetNumberField(TEXT("would_be_index"), Blueprint->NewVariables.Num());
        if (!Tooltip.IsEmpty())
        {
            Entry->SetStringField(TEXT("tooltip"), Tooltip);
        }
        if (Params->HasField(TEXT("default_value")))
        {
            // Stringify whatever shape the caller passed (string / number / bool /
            // object) so the diff records intent without trying to apply
            // type-specific coercion. SetDefaultValue (apply path) handles the
            // type dispatch — that work is skipped in dry-run.
            FString DefaultStr;
            TSharedPtr<FJsonValue> DefaultVal = Params->Values.FindRef("default_value");
            if (DefaultVal.IsValid())
            {
                if (DefaultVal->Type == EJson::String)       DefaultStr = DefaultVal->AsString();
                else if (DefaultVal->Type == EJson::Number)  DefaultStr = FString::SanitizeFloat(DefaultVal->AsNumber());
                else if (DefaultVal->Type == EJson::Boolean) DefaultStr = DefaultVal->AsBool() ? TEXT("true") : TEXT("false");
                else
                {
                    TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&DefaultStr);
                    FJsonSerializer::Serialize(DefaultVal, FString(), JsonWriter);
                }
            }
            Entry->SetStringField(TEXT("default_value"), DefaultStr);
        }

        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
        TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
        Diff->SetArrayField(TEXT("variables_added"), Arr);
        return FMCPCommonUtils::CreateDryRunResponse(Diff);
    }

    if (FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, VarType))
    {
        FBPVariableDescription& Variable = Blueprint->NewVariables.Last();
        Variable.FriendlyName = VariableName;
        Variable.Category = FText::FromString(Category);
        Variable.PropertyFlags = CPF_BlueprintVisible;
        if (IsPublic)
        {
            Variable.PropertyFlags |= CPF_Edit;
        }

        if (!Tooltip.IsEmpty())
        {
            Variable.SetMetaData(FBlueprintMetadata::MD_Tooltip, Tooltip);
        }

        // GAP-016: capture the default-value STRING before the compile below. The
        // FBPVariableDescription::DefaultValue metadata does not reliably survive the
        // recompile (it reads back empty), so we both seed the regenerated CDO and
        // restore the metadata afterward using this captured copy.
        FString CapturedDefault;
        if (Params->HasField(TEXT("default_value")))
        {
            SetDefaultValue(Variable, Params->Values.FindRef("default_value"));
            CapturedDefault = Variable.DefaultValue;
        }

        Blueprint->MarkPackageDirty();

        // Force immediate refresh of the Blueprint editor
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

        // Force asset registry update
        if (GEditor)
        {
            // Note: Asset registry notifications removed for UE5.5 compatibility
            // FAssetRegistryModule::AssetRegistryHelpers::GetAssetRegistry().AssetCreated(Blueprint);

            // Broadcast compilation event to refresh all editors
            // GEditor->BroadcastBlueprintCompiled(Blueprint); // Removed for UE5.5 compatibility

            // Additional refresh for property windows
            FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
            PropertyModule.NotifyCustomizationModuleChanged();
        }

        FKismetEditorUtilities::CompileBlueprint(Blueprint);

        // GAP-016: AddMemberVariable + FBPVariableDescription::DefaultValue alone does
        // not reliably seed the generated CDO (the default reads back empty, because the
        // metadata string does not survive the recompile). Using the value captured
        // before the compile, write it straight onto the fresh CDO property via
        // ImportText (the path bp_set_default_value uses), AND restore the variable
        // description's DefaultValue so subsequent reads/recompiles reflect it.
        if (!CapturedDefault.IsEmpty() && Blueprint->GeneratedClass)
        {
            if (UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject())
            {
                if (FProperty* Prop = Blueprint->GeneratedClass->FindPropertyByName(VarName))
                {
                    void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
                    Prop->ImportText_Direct(*CapturedDefault, ValuePtr, CDO, PPF_None);
                }
            }
            // Restore the metadata string on the (possibly re-created) variable entry.
            for (FBPVariableDescription& Desc : Blueprint->NewVariables)
            {
                if (Desc.VarName == VarName)
                {
                    Desc.DefaultValue = CapturedDefault;
                    break;
                }
            }
        }

        Result->SetBoolField("success", true);

        TSharedPtr<FJsonObject> VarInfo = MakeShared<FJsonObject>();
        VarInfo->SetStringField("name", VariableName);
        VarInfo->SetStringField("type", VariableType);
        VarInfo->SetBoolField("is_public", IsPublic);
        VarInfo->SetStringField("category", Category);

        Result->SetObjectField("variable", VarInfo);
    }
    else
    {
        Result->SetBoolField("success", false);
        Result->SetStringField("error", "Failed to create variable");
    }

    return Result;
}

TSharedPtr<FJsonObject> FBPVariables::DeleteVariable(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    FString BlueprintName = Params->GetStringField(TEXT("blueprint_name"));
    FString VariableName = Params->GetStringField(TEXT("variable_name"));

    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        Result->SetBoolField("success", false);
        Result->SetStringField("error", FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
        return Result;
    }

    if (VariableName.Len() >= NAME_SIZE)
    {
        Result->SetBoolField("success", false);
        Result->SetStringField("error", FString::Printf(TEXT("variable_name is %d characters; FName names are limited to %d"), VariableName.Len(), NAME_SIZE - 1));
        return Result;
    }

    FName VarName = FName(*VariableName);

    // Verify the variable exists before attempting removal.
    bool bFound = false;
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        if (Var.VarName == VarName)
        {
            bFound = true;
            break;
        }
    }

    if (!bFound)
    {
        Result->SetBoolField("success", false);
        Result->SetStringField("error", FString::Printf(TEXT("Variable not found: %s"), *VariableName));
        return Result;
    }

    // RemoveMemberVariable handles node cleanup and compilation.
    FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarName);

    Blueprint->MarkPackageDirty();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    const bool bSuccess = (Blueprint->Status != BS_Error);

    Result->SetBoolField("success", bSuccess);
    if (bSuccess)
    {
        Result->SetStringField("message", FString::Printf(TEXT("Variable '%s' deleted"), *VariableName));
    }
    else
    {
        Result->SetStringField("error", FString::Printf(TEXT("Variable deleted but compilation failed (check Output Log)")));
    }

    return Result;
}

TSharedPtr<FJsonObject> FBPVariables::SetVariableProperties(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    FString BlueprintName = Params->GetStringField(TEXT("blueprint_name"));
    FString VariableName = Params->GetStringField(TEXT("variable_name"));

    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);

    if (!Blueprint)
    {
        Result->SetBoolField("success", false);
        Result->SetStringField("error", FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
        return Result;
    }

    // Find the variable in the Blueprint
    if (VariableName.Len() >= NAME_SIZE)
    {
        Result->SetBoolField("success", false);
        Result->SetStringField("error", FString::Printf(TEXT("variable_name is %d characters; FName names are limited to %d"), VariableName.Len(), NAME_SIZE - 1));
        return Result;
    }
    FBPVariableDescription* VarDesc = nullptr;
    for (FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        if (Var.VarName == FName(*VariableName))
        {
            VarDesc = &Var;
            break;
        }
    }

    if (!VarDesc)
    {
        Result->SetBoolField("success", false);
        Result->SetStringField("error", FString::Printf(TEXT("Variable not found: %s"), *VariableName));
        return Result;
    }

    // dry_run: every preflight ran (field reads, FindBlueprint, NewVariables
    // walk to find VarDesc). Skip the per-field UPROPERTY writes + flag flips +
    // SetMetaData calls + MarkPackageDirty + MarkBlueprintAsModified + Compile.
    // Diff shape per todo/13 phase 2 (mirrors set_node_property's legacy mode
    // and set_material_expression_property's properties_changed[]):
    //   {properties_changed: [{variable_name, property_name, before, after}]}
    // The dispatch tree below has ~20 fields; the dry-run mirror is the same
    // dispatch with capture-instead-of-write. Each field's "before" is read
    // from the existing FBPVariableDescription state (PropertyFlags bits,
    // FriendlyName, Category, GetMetaData(...) lookups, ReplicationCondition).
    // No-op suppression: AddChange skips when before == after, matching the
    // pattern from Phase 3d. Apply still does the redundant write — apply
    // semantics unchanged.
    if (FMCPCommonUtils::ParseDryRun(Params))
    {
        TArray<TSharedPtr<FJsonValue>> Changes;
        auto AddChange = [&](const FString& Name, const FString& Before, const FString& After)
        {
            if (Before == After) return;
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("variable_name"), VariableName);
            O->SetStringField(TEXT("property_name"), Name);
            O->SetStringField(TEXT("before"), Before);
            O->SetStringField(TEXT("after"), After);
            Changes.Add(MakeShared<FJsonValueObject>(O));
        };
        auto BoolStr = [](bool b) { return FString(b ? TEXT("true") : TEXT("false")); };
        auto FlagStr = [&](uint64 Flag) { return BoolStr((VarDesc->PropertyFlags & Flag) != 0); };
        // Read metadata — empty string when key isn't present, mirroring
        // RemoveMetaData's effect (post-remove, GetMetaData returns "").
        auto MetaStr = [&](const TCHAR* Key) -> FString
        {
            FName KeyName(Key);
            for (const FBPVariableMetaDataEntry& E : VarDesc->MetaDataArray)
            {
                if (E.DataKey == KeyName) return E.DataValue;
            }
            return FString();
        };

        if (Params->HasField(TEXT("var_name")))
        {
            AddChange(TEXT("var_name"), VarDesc->VarName.ToString(),
                Params->GetStringField(TEXT("var_name")));
        }
        if (Params->HasField(TEXT("var_type")))
        {
            // Before: emit the raw PinCategory string (and subcategory object
            // class name if present) — full FEdGraphPinType→string round-trip
            // would require the inverse of GetPinTypeFromString which doesn't
            // exist. After: user input. Caller sees current category vs. requested
            // type; they know what type string they passed. For object types the
            // before adds the class to disambiguate (e.g. "object/Actor").
            FString BeforeType = VarDesc->VarType.PinCategory.ToString();
            if (VarDesc->VarType.PinSubCategoryObject.IsValid())
            {
                BeforeType += TEXT("/") + VarDesc->VarType.PinSubCategoryObject->GetName();
            }
            AddChange(TEXT("var_type"), BeforeType, Params->GetStringField(TEXT("var_type")));
        }
        if (Params->HasField(TEXT("is_blueprint_writable")))
        {
            // is_blueprint_writable = NOT CPF_BlueprintReadOnly
            const bool BeforeWritable = (VarDesc->PropertyFlags & CPF_BlueprintReadOnly) == 0;
            AddChange(TEXT("is_blueprint_writable"), BoolStr(BeforeWritable),
                BoolStr(Params->GetBoolField(TEXT("is_blueprint_writable"))));
        }
        if (Params->HasField(TEXT("is_public")))
        {
            AddChange(TEXT("is_public"), FlagStr(CPF_Edit),
                BoolStr(Params->GetBoolField(TEXT("is_public"))));
        }
        if (Params->HasField(TEXT("is_editable_in_instance")))
        {
            const bool BeforeEditable = (VarDesc->PropertyFlags & CPF_DisableEditOnInstance) == 0;
            AddChange(TEXT("is_editable_in_instance"), BoolStr(BeforeEditable),
                BoolStr(Params->GetBoolField(TEXT("is_editable_in_instance"))));
        }
        if (Params->HasField(TEXT("is_config")))
        {
            AddChange(TEXT("is_config"), FlagStr(CPF_Config),
                BoolStr(Params->GetBoolField(TEXT("is_config"))));
        }
        if (Params->HasField(TEXT("friendly_name")))
        {
            AddChange(TEXT("friendly_name"), VarDesc->FriendlyName,
                Params->GetStringField(TEXT("friendly_name")));
        }
        if (Params->HasField(TEXT("tooltip")))
        {
            AddChange(TEXT("tooltip"), MetaStr(TEXT("Tooltip")),
                Params->GetStringField(TEXT("tooltip")));
        }
        if (Params->HasField(TEXT("category")))
        {
            AddChange(TEXT("category"), VarDesc->Category.ToString(),
                Params->GetStringField(TEXT("category")));
        }
        if (Params->HasField(TEXT("replication_enabled")))
        {
            AddChange(TEXT("replication_enabled"), FlagStr(CPF_Net),
                BoolStr(Params->GetBoolField(TEXT("replication_enabled"))));
        }
        if (Params->HasField(TEXT("replication_condition")))
        {
            AddChange(TEXT("replication_condition"),
                FString::FromInt((int32)VarDesc->ReplicationCondition),
                FString::FromInt((int32)Params->GetNumberField(TEXT("replication_condition"))));
        }
        if (Params->HasField(TEXT("is_private")))
        {
            // Before: AllowPrivateAccess metadata == "true" → is_private true.
            const bool BeforePrivate = MetaStr(TEXT("AllowPrivateAccess")) == TEXT("true");
            AddChange(TEXT("is_private"), BoolStr(BeforePrivate),
                BoolStr(Params->GetBoolField(TEXT("is_private"))));
        }
        if (Params->HasField(TEXT("expose_on_spawn")))
        {
            const bool BeforeExpose = MetaStr(TEXT("ExposeOnSpawn")) == TEXT("true");
            AddChange(TEXT("expose_on_spawn"), BoolStr(BeforeExpose),
                BoolStr(Params->GetBoolField(TEXT("expose_on_spawn"))));
        }
        if (Params->HasField(TEXT("default_value")))
        {
            // Before: full FBPVariableDescription default-value reading would
            // require type-aware inspection of DefaultValue (an FString on the
            // variable's CDO) and is non-trivial. For dry-run we emit
            // "<unknown>" and stringify the requested after-value. Callers see
            // what they're requesting; apply does the full SetDefaultValue
            // dispatch. Same compromise documented for create_variable's
            // default_value (Phase 2f).
            FString AfterStr;
            TSharedPtr<FJsonValue> DefaultVal = Params->Values.FindRef("default_value");
            if (DefaultVal.IsValid())
            {
                if (DefaultVal->Type == EJson::String)       AfterStr = DefaultVal->AsString();
                else if (DefaultVal->Type == EJson::Number)  AfterStr = FString::SanitizeFloat(DefaultVal->AsNumber());
                else if (DefaultVal->Type == EJson::Boolean) AfterStr = DefaultVal->AsBool() ? TEXT("true") : TEXT("false");
                else
                {
                    TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&AfterStr);
                    FJsonSerializer::Serialize(DefaultVal, FString(), JsonWriter);
                }
            }
            AddChange(TEXT("default_value"), TEXT("<unknown>"), AfterStr);
        }
        if (Params->HasField(TEXT("expose_to_cinematics")))
        {
            AddChange(TEXT("expose_to_cinematics"), FlagStr(CPF_Interp),
                BoolStr(Params->GetBoolField(TEXT("expose_to_cinematics"))));
        }
        if (Params->HasField(TEXT("slider_range_min")))
        {
            AddChange(TEXT("slider_range_min"), MetaStr(TEXT("UIMin")),
                Params->GetStringField(TEXT("slider_range_min")));
        }
        if (Params->HasField(TEXT("slider_range_max")))
        {
            AddChange(TEXT("slider_range_max"), MetaStr(TEXT("UIMax")),
                Params->GetStringField(TEXT("slider_range_max")));
        }
        if (Params->HasField(TEXT("value_range_min")))
        {
            AddChange(TEXT("value_range_min"), MetaStr(TEXT("ClampMin")),
                Params->GetStringField(TEXT("value_range_min")));
        }
        if (Params->HasField(TEXT("value_range_max")))
        {
            AddChange(TEXT("value_range_max"), MetaStr(TEXT("ClampMax")),
                Params->GetStringField(TEXT("value_range_max")));
        }
        if (Params->HasField(TEXT("units")))
        {
            AddChange(TEXT("units"), MetaStr(TEXT("Units")),
                Params->GetStringField(TEXT("units")));
        }
        if (Params->HasField(TEXT("bitmask")))
        {
            const bool BeforeBitmask = MetaStr(TEXT("Bitmask")) == TEXT("true");
            AddChange(TEXT("bitmask"), BoolStr(BeforeBitmask),
                BoolStr(Params->GetBoolField(TEXT("bitmask"))));
        }
        if (Params->HasField(TEXT("bitmask_enum")))
        {
            AddChange(TEXT("bitmask_enum"), MetaStr(TEXT("BitmaskEnum")),
                Params->GetStringField(TEXT("bitmask_enum")));
        }

        TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
        Diff->SetArrayField(TEXT("properties_changed"), Changes);
        return FMCPCommonUtils::CreateDryRunResponse(Diff);
    }

    // Track which properties were updated
    TSharedPtr<FJsonObject> UpdatedProperties = MakeShared<FJsonObject>();

    // Update var_name (rename variable)
    if (Params->HasField(TEXT("var_name")))
    {
        FString NewVarName = Params->GetStringField(TEXT("var_name"));
        if (NewVarName.Len() >= NAME_SIZE)
        {
            Result->SetBoolField("success", false);
            Result->SetStringField("error", FString::Printf(TEXT("var_name is %d characters; FName names are limited to %d"), NewVarName.Len(), NAME_SIZE - 1));
            return Result;
        }
        VarDesc->VarName = FName(*NewVarName);
        UpdatedProperties->SetStringField("var_name", NewVarName);
    }

    // Update var_type (change variable type)
    if (Params->HasField(TEXT("var_type")))
    {
        FString TypeString = Params->GetStringField(TEXT("var_type"));
        FEdGraphPinType NewType = GetPinTypeFromString(TypeString);
        VarDesc->VarType = NewType;
        UpdatedProperties->SetStringField("var_type", TypeString);
    }

    // Update is_blueprint_writable (Set node)
    if (Params->HasField(TEXT("is_blueprint_writable")))
    {
        bool bIsWritable = Params->GetBoolField(TEXT("is_blueprint_writable"));
        if (bIsWritable)
        {
            VarDesc->PropertyFlags &= ~CPF_BlueprintReadOnly;
        }
        else
        {
            VarDesc->PropertyFlags |= CPF_BlueprintReadOnly;
        }
        UpdatedProperties->SetBoolField("is_blueprint_writable", bIsWritable);
    }

    // Update is_public
    if (Params->HasField(TEXT("is_public")))
    {
        bool bIsPublic = Params->GetBoolField(TEXT("is_public"));
        if (bIsPublic)
        {
            VarDesc->PropertyFlags |= CPF_Edit;
        }
        else
        {
            VarDesc->PropertyFlags &= ~CPF_Edit;
        }
        UpdatedProperties->SetBoolField("is_public", bIsPublic);
    }

    // Update is_editable_in_instance (opposite of CPF_DisableEditOnInstance)
    if (Params->HasField(TEXT("is_editable_in_instance")))
    {
        bool bIsEditable = Params->GetBoolField(TEXT("is_editable_in_instance"));
        if (bIsEditable)
        {
            VarDesc->PropertyFlags &= ~CPF_DisableEditOnInstance;
        }
        else
        {
            VarDesc->PropertyFlags |= CPF_DisableEditOnInstance;
        }
        UpdatedProperties->SetBoolField("is_editable_in_instance", bIsEditable);
    }

    // Update is_config
    if (Params->HasField(TEXT("is_config")))
    {
        bool bIsConfig = Params->GetBoolField(TEXT("is_config"));
        if (bIsConfig)
        {
            VarDesc->PropertyFlags |= CPF_Config;
        }
        else
        {
            VarDesc->PropertyFlags &= ~CPF_Config;
        }
        UpdatedProperties->SetBoolField("is_config", bIsConfig);
    }

    // Update friendly_name
    if (Params->HasField(TEXT("friendly_name")))
    {
        FString FriendlyName = Params->GetStringField(TEXT("friendly_name"));
        VarDesc->FriendlyName = FriendlyName;
        UpdatedProperties->SetStringField("friendly_name", FriendlyName);
    }

    // Update tooltip
    if (Params->HasField(TEXT("tooltip")))
    {
        FString Tooltip = Params->GetStringField(TEXT("tooltip"));
        VarDesc->SetMetaData(FBlueprintMetadata::MD_Tooltip, *Tooltip);
        UpdatedProperties->SetStringField("tooltip", Tooltip);
    }

    // Update category
    if (Params->HasField(TEXT("category")))
    {
        FString Category = Params->GetStringField(TEXT("category"));
        VarDesc->Category = FText::FromString(Category);
        UpdatedProperties->SetStringField("category", Category);
    }

    // Update replication_enabled (Row 15 - CPF_Net flag)
    if (Params->HasField(TEXT("replication_enabled")))
    {
        bool bReplicationEnabled = Params->GetBoolField(TEXT("replication_enabled"));
        if (bReplicationEnabled)
        {
            VarDesc->PropertyFlags |= CPF_Net;
        }
        else
        {
            VarDesc->PropertyFlags &= ~CPF_Net;
        }
        UpdatedProperties->SetBoolField("replication_enabled", bReplicationEnabled);
    }

    // Update replication_condition (Row 16 - ELifetimeCondition)
    if (Params->HasField(TEXT("replication_condition")))
    {
        int32 ReplicationConditionValue = (int32)Params->GetNumberField(TEXT("replication_condition"));
        VarDesc->ReplicationCondition = (ELifetimeCondition)ReplicationConditionValue;
        UpdatedProperties->SetNumberField("replication_condition", ReplicationConditionValue);
    }

    // Update is_private (Row 7 - MD_AllowPrivateAccess metadata)
    if (Params->HasField(TEXT("is_private")))
    {
        bool bIsPrivate = Params->GetBoolField(TEXT("is_private"));
        if (bIsPrivate)
        {
            VarDesc->SetMetaData(TEXT("AllowPrivateAccess"), TEXT("true"));
        }
        else
        {
            VarDesc->RemoveMetaData(TEXT("AllowPrivateAccess"));
        }
        UpdatedProperties->SetBoolField("is_private", bIsPrivate);
    }

    // Update expose_on_spawn (metadata)
    if (Params->HasField(TEXT("expose_on_spawn")))
    {
        bool bExposeOnSpawn = Params->GetBoolField(TEXT("expose_on_spawn"));
        if (bExposeOnSpawn)
        {
            VarDesc->SetMetaData(TEXT("ExposeOnSpawn"), TEXT("true"));
        }
        else
        {
            VarDesc->RemoveMetaData(TEXT("ExposeOnSpawn"));
        }
        UpdatedProperties->SetBoolField("expose_on_spawn", bExposeOnSpawn);
    }

    // Update default_value
    if (Params->HasField(TEXT("default_value")))
    {
        SetDefaultValue(*VarDesc, Params->Values.FindRef("default_value"));
        UpdatedProperties->SetStringField("default_value", "updated");
    }

    // Update expose_to_cinematics (CPF_Interp)
    if (Params->HasField(TEXT("expose_to_cinematics")))
    {
        bool bExposeToCinematics = Params->GetBoolField(TEXT("expose_to_cinematics"));
        if (bExposeToCinematics)
        {
            VarDesc->PropertyFlags |= CPF_Interp;
        }
        else
        {
            VarDesc->PropertyFlags &= ~CPF_Interp;
        }
        UpdatedProperties->SetBoolField("expose_to_cinematics", bExposeToCinematics);
    }

    // Update slider_range_min (MD_UIMin)
    if (Params->HasField(TEXT("slider_range_min")))
    {
        FString SliderMin = Params->GetStringField(TEXT("slider_range_min"));
        VarDesc->SetMetaData(TEXT("UIMin"), *SliderMin);
        UpdatedProperties->SetStringField("slider_range_min", SliderMin);
    }

    // Update slider_range_max (MD_UIMax)
    if (Params->HasField(TEXT("slider_range_max")))
    {
        FString SliderMax = Params->GetStringField(TEXT("slider_range_max"));
        VarDesc->SetMetaData(TEXT("UIMax"), *SliderMax);
        UpdatedProperties->SetStringField("slider_range_max", SliderMax);
    }

    // Update value_range_min (MD_ClampMin)
    if (Params->HasField(TEXT("value_range_min")))
    {
        FString ClampMin = Params->GetStringField(TEXT("value_range_min"));
        VarDesc->SetMetaData(TEXT("ClampMin"), *ClampMin);
        UpdatedProperties->SetStringField("value_range_min", ClampMin);
    }

    // Update value_range_max (MD_ClampMax)
    if (Params->HasField(TEXT("value_range_max")))
    {
        FString ClampMax = Params->GetStringField(TEXT("value_range_max"));
        VarDesc->SetMetaData(TEXT("ClampMax"), *ClampMax);
        UpdatedProperties->SetStringField("value_range_max", ClampMax);
    }

    // Update units (MD_Units)
    if (Params->HasField(TEXT("units")))
    {
        FString Units = Params->GetStringField(TEXT("units"));
        VarDesc->SetMetaData(TEXT("Units"), *Units);
        UpdatedProperties->SetStringField("units", Units);
    }

    // Update bitmask (MD_Bitmask)
    if (Params->HasField(TEXT("bitmask")))
    {
        bool bIsBitmask = Params->GetBoolField(TEXT("bitmask"));
        if (bIsBitmask)
        {
            VarDesc->SetMetaData(TEXT("Bitmask"), TEXT("true"));
        }
        else
        {
            VarDesc->RemoveMetaData(TEXT("Bitmask"));
        }
        UpdatedProperties->SetBoolField("bitmask", bIsBitmask);
    }

    // Update bitmask_enum (MD_BitmaskEnum)
    if (Params->HasField(TEXT("bitmask_enum")))
    {
        FString BitmaskEnum = Params->GetStringField(TEXT("bitmask_enum"));
        VarDesc->SetMetaData(TEXT("BitmaskEnum"), *BitmaskEnum);
        UpdatedProperties->SetStringField("bitmask_enum", BitmaskEnum);
    }

    // Mark Blueprint as modified and compile
    Blueprint->MarkPackageDirty();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    // Force property editor refresh for metadata changes
    // This ensures Details Panel dropdowns (Units, etc.) synchronize with metadata
    if (GEditor)
    {
        FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
        PropertyModule.NotifyCustomizationModuleChanged();
    }

    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    Result->SetBoolField("success", true);
    Result->SetStringField("variable_name", VariableName);
    Result->SetObjectField("properties_updated", UpdatedProperties);
    Result->SetStringField("message", "Variable properties updated successfully");

    return Result;
}

FEdGraphPinType FBPVariables::GetPinTypeFromString(const FString& TypeString)
{
    FEdGraphPinType PinType;

    if (TypeString == "bool")
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    }
    else if (TypeString == "int")
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
    }
    else if (TypeString == "float")
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
        PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    }
    else if (TypeString == "string")
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_String;
    }
    else if (TypeString == "vector")
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
    }
    else if (TypeString == "rotator")
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
    }
    else if (TypeString == "transform")
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        PinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
    }
    else if (TypeString == "byte")
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
    }
    else if (TypeString == "int64")
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
    }
    else if (TypeString == "name")
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
    }
    else if (TypeString == "text")
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
    }
    else
    {
        // Object / struct / enum / class reference (GAP-027 — previously any
        // non-primitive type silently became float). Explicit prefixes pick the
        // family; otherwise auto-probe struct → enum → object class:
        //   "class:Foo"  → TSubclassOf (PC_Class)
        //   "struct:Foo" → struct value (PC_Struct)
        //   "enum:Foo"   → enum byte    (PC_Byte + UEnum)
        // Accepts native names, /Script & /Game paths, and bare Blueprint names.
        enum EForce { Auto, ForceClass, ForceStruct, ForceEnum };
        EForce Mode = Auto;
        FString LookupName = TypeString;
        if (TypeString.StartsWith(TEXT("class:"), ESearchCase::IgnoreCase))       { Mode = ForceClass;  LookupName = TypeString.RightChop(6); }
        else if (TypeString.StartsWith(TEXT("struct:"), ESearchCase::IgnoreCase)) { Mode = ForceStruct; LookupName = TypeString.RightChop(7); }
        else if (TypeString.StartsWith(TEXT("enum:"), ESearchCase::IgnoreCase))   { Mode = ForceEnum;   LookupName = TypeString.RightChop(5); }

        const bool bIsPath = LookupName.StartsWith(TEXT("/"));

        // Note: do NOT use ExactClass — user-defined structs/enums are
        // UUserDefinedStruct / UUserDefinedEnum, not exactly UScriptStruct/UEnum.
        UScriptStruct* ResStruct = nullptr;
        if (Mode == ForceStruct || Mode == Auto)
        {
            ResStruct = bIsPath ? LoadObject<UScriptStruct>(nullptr, *LookupName)
                                : FindFirstObject<UScriptStruct>(*LookupName);
        }

        UEnum* ResEnum = nullptr;
        if (!ResStruct && (Mode == ForceEnum || Mode == Auto))
        {
            ResEnum = bIsPath ? LoadObject<UEnum>(nullptr, *LookupName)
                              : FindFirstObject<UEnum>(*LookupName);
        }

        UClass* ResClass = nullptr;
        if (!ResStruct && !ResEnum && (Mode == ForceClass || Mode == Auto))
        {
            ResClass = FMCPCommonUtils::ResolveClass(LookupName);
        }

        if (ResStruct)
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            PinType.PinSubCategoryObject = ResStruct;
        }
        else if (ResEnum)
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;   // K2 enum vars are byte-backed
            PinType.PinSubCategoryObject = ResEnum;
        }
        else if (ResClass)
        {
            PinType.PinCategory = (Mode == ForceClass)
                ? UEdGraphSchema_K2::PC_Class
                : UEdGraphSchema_K2::PC_Object;
            PinType.PinSubCategoryObject = ResClass;
        }
        else
        {
            // Truly unknown — preserve legacy fallback to float.
            PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
            PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
        }
    }

    return PinType;
}

void FBPVariables::SetDefaultValue(FBPVariableDescription& Variable, const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid())
    {
        return;
    }

    FString StringValue;

    // Convert JSON value to string representation for default value
    if (Value->Type == EJson::String)
    {
        StringValue = Value->AsString();
    }
    else if (Value->Type == EJson::Number)
    {
        StringValue = FString::Printf(TEXT("%g"), Value->AsNumber());
    }
    else if (Value->Type == EJson::Boolean)
    {
        StringValue = Value->AsBool() ? TEXT("true") : TEXT("false");
    }
    else if (Value->Type == EJson::Null)
    {
        StringValue = TEXT("");
    }
    else
    {
        // For complex types, convert to empty string
        StringValue = TEXT("");
    }

    // Update Variable.DefaultValue for Blueprint display
    Variable.DefaultValue = StringValue;
}