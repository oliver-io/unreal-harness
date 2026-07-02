#include "Commands/BlueprintGraph/DispatcherManager.h"
#include "Commands/BlueprintGraph/BPVariables.h"
#include "Commands/MCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_FunctionEntry.h"

// Authors a real Blueprint event dispatcher by replaying the engine's own path,
// FBlueprintEditor::OnAddNewDelegate (UE 5.7, BlueprintEditor.cpp:9606-9652):
//   1. PC_MCDelegate member variable          (AddMemberVariable, line 9624-9626)
//   2. delegate SIGNATURE GRAPH               (CreateNewGraph,    line 9633)
//   3. default nodes + terminators            (lines 9643-9644)
//   4. BlueprintCallable|BlueprintEvent|Public + editable entry (lines 9645-9646)
//   5. register in DelegateSignatureGraphs     (line 9648)
//   6. structural recompile                    (line 9649)
// Typed params are user-defined OUTPUT pins on the signature graph's
// UK2Node_FunctionEntry (data flows OUT of entry → broadcast args), exactly as a
// function INPUT parameter is authored in FFunctionIO::AddFunctionParameter.

namespace
{
    bool ValidateIdentifier(const FString& Name)
    {
        if (Name.IsEmpty() || Name.Len() >= NAME_SIZE)
        {
            return false;
        }
        if (!FChar::IsAlpha(Name[0]) && Name[0] != TEXT('_'))
        {
            return false;
        }
        for (TCHAR Ch : Name)
        {
            if (FChar::IsWhitespace(Ch) || (!FChar::IsAlnum(Ch) && Ch != TEXT('_')))
            {
                return false;
            }
        }
        return true;
    }

    struct FDispatcherParam
    {
        FString Name;
        FString Type;
        bool bIsArray = false;
    };
}

TSharedPtr<FJsonObject> FDispatcherManager::CreateDispatcher(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Invalid parameters"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass a JSON object with `blueprint_name`, `dispatcher_name`, and optional `params` ([{name,type,is_array}])."));
    }

    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'blueprint_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`blueprint_name` is required — the Blueprint asset's short name (e.g. `BP_Player`) or full asset path."));
    }

    FString DispatcherName;
    if (!Params->TryGetStringField(TEXT("dispatcher_name"), DispatcherName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'dispatcher_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("`dispatcher_name` is required — a new identifier for the event dispatcher (C++ identifier rules; PascalCase by convention)."));
    }

    if (!ValidateIdentifier(DispatcherName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Invalid dispatcher_name: must be a valid identifier"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Dispatcher names follow C++ identifier rules — start with a letter/underscore, then letters/digits/underscores; no spaces or punctuation."));
    }

    UBlueprint* Blueprint = FMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName),
            EMCPErrorCode::AssetNotFound,
            TEXT("Verify the Blueprint exists; use `asset_list` or `bp_create_blueprint` first."));
    }

    const FName Name(*DispatcherName);

    // Collision: a member variable or an existing delegate signature graph of the
    // same name. The engine UI uses FindUniqueKismetName to auto-rename; we reject
    // instead so the caller keeps a deterministic name (and a clear error).
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        if (Var.VarName == Name)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("A variable or dispatcher named '%s' already exists on this blueprint"), *DispatcherName),
                EMCPErrorCode::NameCollision,
                TEXT("Pick a different `dispatcher_name`, or remove the existing member first (`bp_delete_variable`)."));
        }
    }
    for (const UEdGraph* SigGraph : Blueprint->DelegateSignatureGraphs)
    {
        if (SigGraph && SigGraph->GetFName() == Name)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("An event dispatcher named '%s' already exists on this blueprint"), *DispatcherName),
                EMCPErrorCode::NameCollision,
                TEXT("Pick a different `dispatcher_name`."));
        }
    }

    // Parse optional typed params.
    TArray<FDispatcherParam> ParamSpecs;
    const TArray<TSharedPtr<FJsonValue>>* ParamsArray = nullptr;
    if (Params->TryGetArrayField(TEXT("params"), ParamsArray) && ParamsArray)
    {
        for (const TSharedPtr<FJsonValue>& Value : *ParamsArray)
        {
            const TSharedPtr<FJsonObject>* ParamObj = nullptr;
            if (!Value.IsValid() || !Value->TryGetObject(ParamObj) || !ParamObj)
            {
                return FMCPCommonUtils::CreateErrorResponse(
                    TEXT("Each entry of 'params' must be an object"),
                    EMCPErrorCode::InvalidArgument,
                    TEXT("`params` is an array of objects, each `{ \"name\": string, \"type\": string, \"is_array\": bool? }`."));
            }

            FDispatcherParam Spec;
            if (!(*ParamObj)->TryGetStringField(TEXT("name"), Spec.Name) ||
                !(*ParamObj)->TryGetStringField(TEXT("type"), Spec.Type))
            {
                return FMCPCommonUtils::CreateErrorResponse(
                    TEXT("Each param needs a 'name' and a 'type'"),
                    EMCPErrorCode::InvalidArgument,
                    TEXT("e.g. { \"name\": \"Score\", \"type\": \"int\" }. `type` uses the same strings as `bp_create_variable`."));
            }
            if (!ValidateIdentifier(Spec.Name))
            {
                return FMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Invalid param name: %s"), *Spec.Name),
                    EMCPErrorCode::InvalidArgument,
                    TEXT("Param names follow C++ identifier rules — letters, digits, underscores; no spaces or punctuation."));
            }
            (*ParamObj)->TryGetBoolField(TEXT("is_array"), Spec.bIsArray);
            ParamSpecs.Add(Spec);
        }
    }

    // dry_run: all preflight ran (field reads, FindBlueprint, collision, param
    // parse). The skipped side effect is AddMemberVariable + signature-graph
    // creation + DelegateSignatureGraphs registration + structural recompile.
    if (FMCPCommonUtils::ParseDryRun(Params))
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
        Entry->SetStringField(TEXT("dispatcher_name"), DispatcherName);

        TArray<TSharedPtr<FJsonValue>> ParamJson;
        for (const FDispatcherParam& Spec : ParamSpecs)
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("name"), Spec.Name);
            P->SetStringField(TEXT("type"), Spec.Type);
            P->SetBoolField(TEXT("is_array"), Spec.bIsArray);
            ParamJson.Add(MakeShared<FJsonValueObject>(P));
        }
        Entry->SetArrayField(TEXT("params"), ParamJson);

        TArray<TSharedPtr<FJsonValue>> Added;
        Added.Add(MakeShared<FJsonValueObject>(Entry));
        TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
        Diff->SetArrayField(TEXT("dispatchers_added"), Added);
        return FMCPCommonUtils::CreateDryRunResponse(Diff);
    }

    // ── Apply — replay FBlueprintEditor::OnAddNewDelegate ───────────────────
    const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
    Blueprint->Modify();

    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;  // signature resolved by name == graph name at compile
    if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, Name, DelegateType))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create dispatcher variable '%s'"), *DispatcherName),
            EMCPErrorCode::Internal,
            TEXT("FBlueprintEditorUtils::AddMemberVariable returned false (reserved name or duplicate). Check the editor log."));
    }

    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!NewGraph)
    {
        // Roll back the member variable so we don't leave an orphaned PC_MCDelegate.
        FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, Name);
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to create signature graph for dispatcher '%s'"), *DispatcherName),
            EMCPErrorCode::Internal,
            TEXT("FBlueprintEditorUtils::CreateNewGraph returned null. Check the editor log."));
    }

    NewGraph->bEditable = false;
    K2Schema->CreateDefaultNodesForGraph(*NewGraph);
    K2Schema->CreateFunctionGraphTerminators(*NewGraph, static_cast<UClass*>(nullptr));
    K2Schema->AddExtraFunctionFlags(NewGraph, (FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public));
    K2Schema->MarkFunctionEntryAsEditable(NewGraph, true);

    Blueprint->DelegateSignatureGraphs.Add(NewGraph);

    // Typed params → user-defined OUTPUT pins on the signature FunctionEntry.
    int32 ParamsAdded = 0;
    if (ParamSpecs.Num() > 0)
    {
        UK2Node_FunctionEntry* EntryNode = nullptr;
        for (UEdGraphNode* Node : NewGraph->Nodes)
        {
            if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
            {
                EntryNode = Entry;
                break;
            }
        }

        if (EntryNode)
        {
            for (const FDispatcherParam& Spec : ParamSpecs)
            {
                FEdGraphPinType PinType = FBPVariables::GetPinTypeFromString(Spec.Type);
                if (Spec.bIsArray)
                {
                    PinType.ContainerType = EPinContainerType::Array;
                }
                if (EntryNode->CreateUserDefinedPin(FName(*Spec.Name), PinType, EGPD_Output))
                {
                    ++ParamsAdded;
                }
            }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("dispatcher_name"), DispatcherName);
    Result->SetStringField(TEXT("graph_id"), NewGraph->GetFName().ToString());
    Result->SetNumberField(TEXT("params_added"), ParamsAdded);
    return Result;
}
