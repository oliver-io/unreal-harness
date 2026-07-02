#include "Commands/MCPReflectionCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "Engine/Blueprint.h"
#include "Engine/UserDefinedEnum.h"
#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node_CallFunction.h"
#include "Misc/PackageName.h"

namespace
{
    // Resolve a UEnum from a flexible name input. Accepts:
    //   /Script/Module.EnumName  (fully qualified)
    //   /Game/Path/UDE_Foo       (UserDefinedEnum asset path)
    //   EBlendMode               (short name as-given)
    //   BlendMode                (short name; tries with E-prefix as fallback)
    UEnum* ResolveEnum(const FString& Name)
    {
        if (Name.IsEmpty())
        {
            return nullptr;
        }

        // Asset path form (UserDefinedEnum) or fully-qualified script path.
        if (Name.StartsWith(TEXT("/")))
        {
            if (UEnum* Loaded = LoadObject<UEnum>(nullptr, *Name))
            {
                return Loaded;
            }
        }

        if (UEnum* Found = FindFirstObject<UEnum>(*Name, EFindFirstObjectOptions::ExactClass))
        {
            return Found;
        }

        if (!Name.StartsWith(TEXT("E")) && !Name.Contains(TEXT(".")))
        {
            const FString WithEPrefix = FString::Printf(TEXT("E%s"), *Name);
            if (UEnum* Found = FindFirstObject<UEnum>(*WithEPrefix, EFindFirstObjectOptions::ExactClass))
            {
                return Found;
            }
        }

        return nullptr;
    }

    // Resolve a UClass from a flexible name input. Mirrors HandleGetClassProperties.
    UClass* ResolveClass(const FString& Name)
    {
        if (Name.IsEmpty())
        {
            return nullptr;
        }

        if (Name.StartsWith(TEXT("/Script/")))
        {
            if (UClass* Loaded = LoadClass<UObject>(nullptr, *Name))
            {
                return Loaded;
            }
        }

        if (UClass* Found = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::ExactClass))
        {
            return Found;
        }

        if (Name.StartsWith(TEXT("U")) && Name.Len() > 1)
        {
            if (UClass* Found = FindFirstObject<UClass>(*Name.RightChop(1), EFindFirstObjectOptions::ExactClass))
            {
                return Found;
            }
        }
        else if (!Name.StartsWith(TEXT("U")) && !Name.StartsWith(TEXT("A")))
        {
            const FString WithUPrefix = FString::Printf(TEXT("U%s"), *Name);
            if (UClass* Found = FindFirstObject<UClass>(*WithUPrefix, EFindFirstObjectOptions::ExactClass))
            {
                return Found;
            }
        }

        return nullptr;
    }

    // Hidden / HideDropDown classes are noise for the agent — abstract base helpers,
    // deprecated stubs, etc. Filtered by default; opt in with include_hidden=true.
    bool IsClassHiddenForAgent(const UClass* Cls)
    {
        if (!Cls)
        {
            return true;
        }
        if (Cls->HasAnyClassFlags(CLASS_Hidden | CLASS_HideDropDown | CLASS_Deprecated | CLASS_NewerVersionExists))
        {
            return true;
        }
        return false;
    }
}

TSharedPtr<FJsonObject> FMCPReflectionCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("enum_inspect"))
    {
        return HandleEnumInspect(Params);
    }
    if (CommandType == TEXT("class_query"))
    {
        return HandleClassQuery(Params);
    }
    if (CommandType == TEXT("bp_function_references"))
    {
        return HandleBpFunctionReferences(Params);
    }
    if (CommandType == TEXT("class_inspect"))
    {
        return HandleClassInspect(Params);
    }
    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown reflection command: %s"), *CommandType),
        EMCPErrorCode::InvalidArgument,
        TEXT("Supported reflection commands in this build: enum_inspect, class_query, bp_function_references, class_inspect."));
}

TSharedPtr<FJsonObject> FMCPReflectionCommands::HandleEnumInspect(const TSharedPtr<FJsonObject>& Params)
{
    FString EnumName;
    if (!Params->TryGetStringField(TEXT("enum_name"), EnumName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'enum_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass 'enum_name' as a fully-qualified path (\"/Script/Engine.EBlendMode\"), an asset path for UserDefinedEnums, or a short name (\"EBlendMode\" or \"BlendMode\")."));
    }

    UEnum* TargetEnum = ResolveEnum(EnumName);
    if (!TargetEnum)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Enum not found: %s"), *EnumName),
            EMCPErrorCode::AssetNotFound,
            TEXT("Verify the spelling and namespace. UserDefinedEnums live under /Game/...; native enums under /Script/<Module>."));
    }

    TArray<TSharedPtr<FJsonValue>> MemberArray;

    // NumEnums() includes the synthetic _MAX sentinel that UENUMs gain by default.
    // Exclude it from the wire output — agents reasoning over members never want
    // to enumerate _MAX, and including it fakes a member that has no semantic value.
    const int32 NumEntries = TargetEnum->NumEnums();
    for (int32 i = 0; i < NumEntries; ++i)
    {
        const FString ShortName = TargetEnum->GetNameStringByIndex(i);
        if (ShortName.EndsWith(TEXT("_MAX")))
        {
            continue;
        }

        TSharedPtr<FJsonObject> Member = MakeShared<FJsonObject>();
        Member->SetStringField(TEXT("name"), ShortName);
        Member->SetStringField(TEXT("display_name"), TargetEnum->GetDisplayNameTextByIndex(i).ToString());
        Member->SetNumberField(TEXT("value"), static_cast<double>(TargetEnum->GetValueByIndex(i)));

#if WITH_EDITORONLY_DATA
        // Metadata is editor-only on UEnum. The MCP runs only in the editor, but the
        // guard keeps this file portable if it ever links into a non-editor target.
        const FString ToolTip = TargetEnum->GetMetaData(TEXT("ToolTip"), i);
        if (!ToolTip.IsEmpty())
        {
            Member->SetStringField(TEXT("tooltip"), ToolTip);
        }
        const bool bHidden = TargetEnum->HasMetaData(TEXT("Hidden"), i);
        Member->SetBoolField(TEXT("is_hidden"), bHidden);
#endif

        MemberArray.Add(MakeShared<FJsonValueObject>(Member));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("enum_name"), TargetEnum->GetName());
    ResultObj->SetStringField(TEXT("enum_path"), TargetEnum->GetPathName());
    ResultObj->SetStringField(TEXT("cpp_form"),
        TargetEnum->GetCppForm() == UEnum::ECppForm::EnumClass    ? TEXT("enum_class")
      : TargetEnum->GetCppForm() == UEnum::ECppForm::Namespaced   ? TEXT("namespaced")
      :                                                              TEXT("regular"));
    ResultObj->SetBoolField(TEXT("is_user_defined"), TargetEnum->IsA<UUserDefinedEnum>());
    ResultObj->SetArrayField(TEXT("members"), MemberArray);
    ResultObj->SetNumberField(TEXT("member_count"), MemberArray.Num());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPReflectionCommands::HandleClassQuery(const TSharedPtr<FJsonObject>& Params)
{
    FString NamePattern;
    Params->TryGetStringField(TEXT("name_pattern"), NamePattern);

    FString ParentName;
    const bool bHasParent = Params->TryGetStringField(TEXT("parent"), ParentName);

    bool bRecursive = false;
    Params->TryGetBoolField(TEXT("recursive"), bRecursive);

    bool bIncludeHidden = false;
    Params->TryGetBoolField(TEXT("include_hidden"), bIncludeHidden);

    int32 Cursor = 0;
    Params->TryGetNumberField(TEXT("cursor"), Cursor);
    if (Cursor < 0)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("'cursor' must be non-negative"),
            EMCPErrorCode::OutOfRange,
            TEXT("Pass the 'next_cursor' value from a previous response, or omit for the first page."));
    }

    int32 Limit = 200;
    Params->TryGetNumberField(TEXT("limit"), Limit);
    if (Limit <= 0 || Limit > 1000)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("'limit' must be between 1 and 1000"),
            EMCPErrorCode::OutOfRange,
            TEXT("Default limit is 200; the upper bound prevents accidentally pulling the entire class registry."));
    }

    UClass* ParentClass = nullptr;
    if (bHasParent)
    {
        ParentClass = ResolveClass(ParentName);
        if (!ParentClass)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Parent class not found: %s"), *ParentName),
                EMCPErrorCode::ClassNotLoaded,
                TEXT("Pass a fully-qualified path (\"/Script/Engine.Actor\"), or a short name (\"AActor\" / \"Actor\"). Class must be loaded — call class_inspect or load the owning module first."));
        }
    }

    // Collect candidate classes.
    TArray<UClass*> Candidates;
    if (ParentClass)
    {
        TArray<UClass*> Derived;
        GetDerivedClasses(ParentClass, Derived, bRecursive);
        Candidates = MoveTemp(Derived);
    }
    else
    {
        // Live registered UClasses only. Unloaded (asset-registry-only) Blueprint classes
        // are not visible here — that's a deliberate scope cut for this iteration; the
        // unloaded path lands with the asset_references doc (#4).
        for (TObjectIterator<UClass> It; It; ++It)
        {
            Candidates.Add(*It);
        }
    }

    // Apply name + visibility filters.
    const bool bHasNameFilter = !NamePattern.IsEmpty();
    TArray<UClass*> Filtered;
    Filtered.Reserve(Candidates.Num());
    for (UClass* Cls : Candidates)
    {
        if (!Cls)
        {
            continue;
        }
        if (!bIncludeHidden && IsClassHiddenForAgent(Cls))
        {
            continue;
        }
        if (bHasNameFilter && !Cls->GetName().Contains(NamePattern, ESearchCase::IgnoreCase))
        {
            continue;
        }
        Filtered.Add(Cls);
    }

    // Stable sort by path so cursor pagination is deterministic across calls.
    Filtered.Sort([](const UClass& A, const UClass& B) { return A.GetPathName() < B.GetPathName(); });

    const int32 TotalMatched = Filtered.Num();
    const int32 PageEnd = FMath::Min(Cursor + Limit, TotalMatched);

    TArray<TSharedPtr<FJsonValue>> ClassArray;
    ClassArray.Reserve(FMath::Max(0, PageEnd - Cursor));
    for (int32 i = Cursor; i < PageEnd; ++i)
    {
        UClass* Cls = Filtered[i];
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Cls->GetName());
        Entry->SetStringField(TEXT("path"), Cls->GetPathName());
        if (UClass* Super = Cls->GetSuperClass())
        {
            Entry->SetStringField(TEXT("parent_class"), Super->GetPathName());
        }
        Entry->SetBoolField(TEXT("is_blueprint"), Cls->ClassGeneratedBy != nullptr);
        Entry->SetBoolField(TEXT("is_abstract"), Cls->HasAnyClassFlags(CLASS_Abstract));
        Entry->SetBoolField(TEXT("is_native"), Cls->HasAnyClassFlags(CLASS_Native));
        ClassArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("classes"), ClassArray);
    ResultObj->SetNumberField(TEXT("returned_count"), ClassArray.Num());
    ResultObj->SetNumberField(TEXT("total_matched"), TotalMatched);
    ResultObj->SetNumberField(TEXT("cursor"), Cursor);
    if (PageEnd < TotalMatched)
    {
        ResultObj->SetNumberField(TEXT("next_cursor"), PageEnd);
    }
    if (bHasParent)
    {
        ResultObj->SetStringField(TEXT("parent"), ParentClass->GetPathName());
        ResultObj->SetBoolField(TEXT("recursive"), bRecursive);
    }
    if (bHasNameFilter)
    {
        ResultObj->SetStringField(TEXT("name_pattern"), NamePattern);
    }
    ResultObj->SetBoolField(TEXT("include_hidden"), bIncludeHidden);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

namespace
{
    UBlueprint* LoadBlueprintFromPath(const FString& Path)
    {
        if (Path.IsEmpty()) return nullptr;
        if (UObject* Loaded = UEditorAssetLibrary::LoadAsset(Path))
        {
            return Cast<UBlueprint>(Loaded);
        }
        return nullptr;
    }

    // Build a JSON entry for a single K2Node_CallFunction reference.
    // counterpart_function: full path (Class.Function) of the function being called.
    // blueprint_path: which BP contains this calling node.
    // graph_name / node_id / node_name: location of the call.
    TSharedPtr<FJsonObject> CallNodeToJson(UK2Node_CallFunction* CallNode)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        if (!CallNode) return Entry;

        // Counterpart (the function being called).
        const FName MemberName = CallNode->FunctionReference.GetMemberName();
        UClass* MemberParent = CallNode->FunctionReference.GetMemberParentClass();
        if (MemberParent)
        {
            Entry->SetStringField(TEXT("counterpart_class"), MemberParent->GetPathName());
            Entry->SetStringField(TEXT("counterpart_function"),
                FString::Printf(TEXT("%s::%s"), *MemberParent->GetPathName(), *MemberName.ToString()));
        }
        else
        {
            // Self-referential — call to a function on the same BP class
            Entry->SetStringField(TEXT("counterpart_function"), MemberName.ToString());
        }

        // Location.
        if (UEdGraph* Graph = Cast<UEdGraph>(CallNode->GetOuter()))
        {
            Entry->SetStringField(TEXT("graph_name"), Graph->GetName());
        }
        Entry->SetStringField(TEXT("node_id"), CallNode->NodeGuid.ToString());
        Entry->SetStringField(TEXT("node_name"), CallNode->GetName());

        // Containing blueprint.
        if (UBlueprint* OwningBP = Cast<UBlueprint>(CallNode->GetBlueprint()))
        {
            Entry->SetStringField(TEXT("blueprint_path"), OwningBP->GetPathName());
        }
        return Entry;
    }
}

TSharedPtr<FJsonObject> FMCPReflectionCommands::HandleBpFunctionReferences(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    if (!Params->TryGetStringField(TEXT("bp_path"), BlueprintPath) &&
        !Params->TryGetStringField(TEXT("blueprint_path"), BlueprintPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'bp_path' (or legacy 'blueprint_path') parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the asset path of the Blueprint that owns or calls the function."));
    }

    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'function_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the function's short name (FName), not a fully-qualified path."));
    }

    FString DirectionStr;
    if (!Params->TryGetStringField(TEXT("direction"), DirectionStr) || DirectionStr.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'direction' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Closed set: 'callees' (what does this function call?) or 'callers' (who calls this function?)."));
    }
    const FString DirectionLower = DirectionStr.ToLower();
    const bool bCallees = DirectionLower == TEXT("callees") || DirectionLower == TEXT("outbound");
    const bool bCallers = DirectionLower == TEXT("callers") || DirectionLower == TEXT("inbound");
    if (!bCallees && !bCallers)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown 'direction' value: %s"), *DirectionStr),
            EMCPErrorCode::InvalidArgument,
            TEXT("Closed set: 'callees' (alias 'outbound') or 'callers' (alias 'inbound')."));
    }

    UBlueprint* OwningBP = LoadBlueprintFromPath(BlueprintPath);
    if (!OwningBP)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("Verify the path. Use list_assets to scan the content tree if needed."));
    }

    TArray<TSharedPtr<FJsonValue>> ResultsArr;

    if (bCallees)
    {
        // Find the function graph by name on the BP.
        UEdGraph* TargetGraph = nullptr;
        for (UEdGraph* G : OwningBP->FunctionGraphs)
        {
            if (G && G->GetName() == FunctionName)
            {
                TargetGraph = G;
                break;
            }
        }
        if (!TargetGraph)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Function not found on Blueprint: %s"), *FunctionName),
                EMCPErrorCode::FunctionNotFound,
                TEXT("Pass the BP function's short name (case-sensitive). bp_brief lists available graphs."));
        }

        // Walk every K2Node_CallFunction in the graph.
        for (UEdGraphNode* N : TargetGraph->Nodes)
        {
            if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(N))
            {
                ResultsArr.Add(MakeShared<FJsonValueObject>(CallNodeToJson(Call)));
            }
        }
    }
    else
    {
        // Inbound: iterate every loaded UBlueprint, walk its graphs, find K2Node_CallFunction
        // nodes whose FunctionReference resolves to the input function name on the input BP's
        // generated class. This is O(loaded_BPs × graphs × nodes); fine for project-scale scans.
        UClass* OwningClass = OwningBP->GeneratedClass;
        if (!OwningClass)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Blueprint has no GeneratedClass: %s"), *BlueprintPath),
                EMCPErrorCode::Internal,
                TEXT("Recompile the Blueprint and retry."));
        }
        const FName TargetFnName(*FunctionName);

        for (TObjectIterator<UBlueprint> It; It; ++It)
        {
            UBlueprint* Other = *It;
            if (!Other) continue;

            TArray<UEdGraph*> AllGraphs;
            Other->GetAllGraphs(AllGraphs);
            for (UEdGraph* G : AllGraphs)
            {
                if (!G) continue;
                for (UEdGraphNode* N : G->Nodes)
                {
                    UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(N);
                    if (!Call) continue;
                    if (Call->FunctionReference.GetMemberName() != TargetFnName) continue;

                    // Resolve parent class. If no explicit parent (self call), it implicitly
                    // belongs to the calling BP's own class — count those when the calling BP IS
                    // the owning BP (a self-referential call inside the same BP).
                    UClass* CallParent = Call->FunctionReference.GetMemberParentClass();
                    if (!CallParent)
                    {
                        if (Other != OwningBP) continue;
                    }
                    else if (CallParent != OwningClass &&
                             !OwningClass->IsChildOf(CallParent) &&
                             !CallParent->IsChildOf(OwningClass))
                    {
                        // Different class hierarchy — not a reference to our target function.
                        continue;
                    }
                    ResultsArr.Add(MakeShared<FJsonValueObject>(CallNodeToJson(Call)));
                }
            }
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("bp_path"), OwningBP->GetPathName());
    ResultObj->SetStringField(TEXT("function_name"), FunctionName);
    ResultObj->SetStringField(TEXT("direction"), bCallees ? TEXT("callees") : TEXT("callers"));
    ResultObj->SetArrayField(TEXT("references"), ResultsArr);
    ResultObj->SetNumberField(TEXT("count"), ResultsArr.Num());
    if (bCallers)
    {
        ResultObj->SetBoolField(TEXT("scanned_loaded_blueprints_only"), true);
    }
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

namespace
{
    void EmitClassProperties(UClass* Cls, bool bIncludeInherited, TArray<TSharedPtr<FJsonValue>>& OutArr)
    {
        const EFieldIteratorFlags::SuperClassFlags SuperFlag = bIncludeInherited
            ? EFieldIteratorFlags::IncludeSuper
            : EFieldIteratorFlags::ExcludeSuper;

        for (TFieldIterator<FProperty> It(Cls, SuperFlag); It; ++It)
        {
            FProperty* Prop = *It;
            if (!Prop) continue;

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Prop->GetName());

            FString ExtendedType;
            FString CppType = Prop->GetCPPType(&ExtendedType, CPPF_None);
            if (!ExtendedType.IsEmpty()) CppType += ExtendedType;
            Entry->SetStringField(TEXT("type"), CppType);

            if (UClass* OwnerClass = Prop->GetOwnerClass())
            {
                Entry->SetStringField(TEXT("declared_in"), OwnerClass->GetPathName());
            }
            Entry->SetBoolField(TEXT("is_blueprint_visible"),  Prop->HasAnyPropertyFlags(CPF_BlueprintVisible));
            Entry->SetBoolField(TEXT("is_blueprint_writable"), !Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly));
            Entry->SetBoolField(TEXT("is_editable"),           Prop->HasAnyPropertyFlags(CPF_Edit));
            Entry->SetBoolField(TEXT("is_replicated"),         Prop->HasAnyPropertyFlags(CPF_Net));

            OutArr.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    void EmitClassFunctions(UClass* Cls, bool bIncludeInherited, TArray<TSharedPtr<FJsonValue>>& OutArr)
    {
        const EFieldIteratorFlags::SuperClassFlags SuperFlag = bIncludeInherited
            ? EFieldIteratorFlags::IncludeSuper
            : EFieldIteratorFlags::ExcludeSuper;

        for (TFieldIterator<UFunction> It(Cls, SuperFlag); It; ++It)
        {
            UFunction* Fn = *It;
            if (!Fn) continue;

            TSharedPtr<FJsonObject> FnObj = MakeShared<FJsonObject>();
            FnObj->SetStringField(TEXT("name"), Fn->GetName());
            FnObj->SetStringField(TEXT("path"), Fn->GetPathName());
            if (UClass* Owner = Fn->GetOwnerClass())
            {
                FnObj->SetStringField(TEXT("declared_in"), Owner->GetPathName());
            }

            FnObj->SetBoolField(TEXT("is_blueprint_callable"), Fn->HasAnyFunctionFlags(FUNC_BlueprintCallable));
            FnObj->SetBoolField(TEXT("is_blueprint_pure"),     Fn->HasAnyFunctionFlags(FUNC_BlueprintPure));
            FnObj->SetBoolField(TEXT("is_static"),             Fn->HasAnyFunctionFlags(FUNC_Static));
            FnObj->SetBoolField(TEXT("is_native"),             Fn->HasAnyFunctionFlags(FUNC_Native));
            FnObj->SetBoolField(TEXT("is_event"),              Fn->HasAnyFunctionFlags(FUNC_Event));
            FnObj->SetBoolField(TEXT("is_net"),                Fn->HasAnyFunctionFlags(FUNC_Net));

            // Parameter list: each FProperty with CPF_Parm but not CPF_ReturnParm.
            // Return type: the property with CPF_ReturnParm (if any).
            TArray<TSharedPtr<FJsonValue>> ParamsArr;
            FString ReturnType;
            for (TFieldIterator<FProperty> ParamIt(Fn); ParamIt; ++ParamIt)
            {
                FProperty* Prop = *ParamIt;
                if (!Prop || !Prop->HasAnyPropertyFlags(CPF_Parm)) continue;

                FString ExtType;
                FString CppType = Prop->GetCPPType(&ExtType, CPPF_None);
                if (!ExtType.IsEmpty()) CppType += ExtType;

                if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
                {
                    ReturnType = CppType;
                    continue;
                }

                TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
                ParamObj->SetStringField(TEXT("name"), Prop->GetName());
                ParamObj->SetStringField(TEXT("type"), CppType);
                ParamObj->SetBoolField(TEXT("is_out"),       Prop->HasAnyPropertyFlags(CPF_OutParm) && !Prop->HasAnyPropertyFlags(CPF_ConstParm));
                ParamObj->SetBoolField(TEXT("is_const"),     Prop->HasAnyPropertyFlags(CPF_ConstParm));
                ParamObj->SetBoolField(TEXT("is_reference"), Prop->HasAnyPropertyFlags(CPF_ReferenceParm));
                ParamsArr.Add(MakeShared<FJsonValueObject>(ParamObj));
            }
            FnObj->SetArrayField(TEXT("parameters"), ParamsArr);
            FnObj->SetStringField(TEXT("return_type"), ReturnType.IsEmpty() ? TEXT("void") : ReturnType);

            OutArr.Add(MakeShared<FJsonValueObject>(FnObj));
        }
    }

    void EmitClassHierarchy(UClass* Cls, TArray<TSharedPtr<FJsonValue>>& OutArr)
    {
        // Walk parent chain inclusive of the class itself, root last (UObject at end).
        UClass* Cur = Cls;
        while (Cur)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("class"), Cur->GetPathName());
            Entry->SetStringField(TEXT("name"),  Cur->GetName());
            OutArr.Add(MakeShared<FJsonValueObject>(Entry));
            Cur = Cur->GetSuperClass();
        }
    }
}

TSharedPtr<FJsonObject> FMCPReflectionCommands::HandleClassInspect(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassName;
    if (!Params->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'class_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass a UClass path or short name. class_query produces the path."));
    }

    UClass* TargetClass = ResolveClass(ClassName);
    if (!TargetClass)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class not found: %s"), *ClassName),
            EMCPErrorCode::ClassNotLoaded,
            TEXT("Pass a fully-qualified path (\"/Script/Engine.Actor\"), an asset path, or a short name. The class must be loaded; class_query without a parent filter shows all loaded classes."));
    }

    // include parameter — array of strings selecting which sections to emit.
    // Default: ['properties'] for back-compat semantics with get_class_properties.
    TSet<FString> IncludeSet;
    const TArray<TSharedPtr<FJsonValue>>* IncludeArr;
    if (Params->TryGetArrayField(TEXT("include"), IncludeArr))
    {
        for (const TSharedPtr<FJsonValue>& V : *IncludeArr)
        {
            if (V.IsValid() && V->Type == EJson::String)
            {
                IncludeSet.Add(V->AsString().ToLower());
            }
        }
    }
    if (IncludeSet.IsEmpty())
    {
        IncludeSet.Add(TEXT("properties"));
    }

    bool bIncludeInherited = false;
    Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("class_name"), TargetClass->GetName());
    ResultObj->SetStringField(TEXT("class_path"), TargetClass->GetPathName());
    if (UClass* Super = TargetClass->GetSuperClass())
    {
        ResultObj->SetStringField(TEXT("parent_class"), Super->GetPathName());
    }
    ResultObj->SetBoolField(TEXT("is_blueprint"), TargetClass->ClassGeneratedBy != nullptr);
    ResultObj->SetBoolField(TEXT("is_abstract"),  TargetClass->HasAnyClassFlags(CLASS_Abstract));
    ResultObj->SetBoolField(TEXT("is_native"),    TargetClass->HasAnyClassFlags(CLASS_Native));
    ResultObj->SetBoolField(TEXT("include_inherited"), bIncludeInherited);

    TArray<TSharedPtr<FJsonValue>> IncludeEcho;
    for (const FString& I : IncludeSet) IncludeEcho.Add(MakeShared<FJsonValueString>(I));
    ResultObj->SetArrayField(TEXT("include"), IncludeEcho);

    if (IncludeSet.Contains(TEXT("properties")))
    {
        TArray<TSharedPtr<FJsonValue>> Props;
        EmitClassProperties(TargetClass, bIncludeInherited, Props);
        ResultObj->SetArrayField(TEXT("properties"), Props);
        ResultObj->SetNumberField(TEXT("properties_count"), Props.Num());
    }

    if (IncludeSet.Contains(TEXT("functions")))
    {
        TArray<TSharedPtr<FJsonValue>> Funcs;
        EmitClassFunctions(TargetClass, bIncludeInherited, Funcs);
        ResultObj->SetArrayField(TEXT("functions"), Funcs);
        ResultObj->SetNumberField(TEXT("functions_count"), Funcs.Num());
    }

    if (IncludeSet.Contains(TEXT("hierarchy")))
    {
        TArray<TSharedPtr<FJsonValue>> Hier;
        EmitClassHierarchy(TargetClass, Hier);
        ResultObj->SetArrayField(TEXT("hierarchy"), Hier);
        ResultObj->SetNumberField(TEXT("hierarchy_depth"), Hier.Num());
    }

    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}
