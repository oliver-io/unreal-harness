#include "StateTreeBindingMgr.h"
#include "StateTreeTypeCache.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeEditorPropertyBindings.h"
#include "PropertyBindingPath.h"
#include "InstancedStruct.h"
#include "MCPCommonUtils.h"

// ---- AddPropertyBinding ----

TSharedPtr<FJsonObject> FStateTreeBindingMgr::AddPropertyBinding(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	// Required params
	FString SourceNodeId, SourceProperty, TargetNodeId, TargetProperty;
	if (!Params->TryGetStringField(TEXT("source_node_id"), SourceNodeId))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'source_node_id' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `source_node_id` (string GUID) — the node providing the bound value."));
	if (!Params->TryGetStringField(TEXT("source_property"), SourceProperty))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'source_property' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `source_property` (string) — dotted path of the source property (e.g. \"Output.Value\")."));
	if (!Params->TryGetStringField(TEXT("target_node_id"), TargetNodeId))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'target_node_id' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `target_node_id` (string GUID) — the node receiving the bound value. Use `statetree_state_list` / `statetree_read` to discover."));
	if (!Params->TryGetStringField(TEXT("target_property"), TargetProperty))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'target_property' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `target_property` (string) — dotted path of the property to bind to (e.g. \"Range.Min\"). Use `statetree_binding_list_bindable` to discover bindable properties."));

	// Parse GUIDs
	FGuid SourceGuid, TargetGuid;
	if (!FGuid::Parse(SourceNodeId, SourceGuid))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid source_node_id GUID"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`source_node_id` must be a parseable FGuid string."));
	if (!FGuid::Parse(TargetNodeId, TargetGuid))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid target_node_id GUID"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`target_node_id` must be a parseable FGuid string (e.g. \"12345678-1234-1234-1234-123456789012\")."));

	// Construct property paths using the correct API
	FPropertyBindingPath SourcePath;
	SourcePath.SetStructID(SourceGuid);
	TArray<FString> SourceSegments;
	SourceProperty.ParseIntoArray(SourceSegments, TEXT("."));
	for (const FString& Segment : SourceSegments)
	{
		if (Segment.Len() >= NAME_SIZE)
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("source_property path segment too long for FName"),
				EMCPErrorCode::InvalidArgument,
				TEXT("Each dotted path segment of `source_property` must be shorter than 1024 characters."));
		SourcePath.AddPathSegment(FName(*Segment));
	}

	FPropertyBindingPath TargetPath;
	TargetPath.SetStructID(TargetGuid);
	TArray<FString> TargetSegments;
	TargetProperty.ParseIntoArray(TargetSegments, TEXT("."));
	for (const FString& Segment : TargetSegments)
	{
		if (Segment.Len() >= NAME_SIZE)
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("target_property path segment too long for FName"),
				EMCPErrorCode::InvalidArgument,
				TEXT("Each dotted path segment of `target_property` must be shorter than 1024 characters."));
		TargetPath.AddPathSegment(FName(*Segment));
	}

	// Modify() the EditorData before writing to EditorBindings — FStateTreeEditorPropertyBindings
	// is a UPROPERTY struct on UStateTreeEditorData, so the EditorData is the
	// transactional scope. Pre-fix binding adds left no package-level dirty flag.
	EditorData->Modify();

	// Add the binding
	EditorData->EditorBindings.AddBinding(SourcePath, TargetPath);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("source"), FString::Printf(TEXT("%s.%s"), *SourceNodeId, *SourceProperty));
	Result->SetStringField(TEXT("target"), FString::Printf(TEXT("%s.%s"), *TargetNodeId, *TargetProperty));
	return Result;
}

// ---- RemovePropertyBinding ----

TSharedPtr<FJsonObject> FStateTreeBindingMgr::RemovePropertyBinding(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	FString TargetNodeId, TargetProperty;
	if (!Params->TryGetStringField(TEXT("target_node_id"), TargetNodeId))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'target_node_id' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `target_node_id` (string GUID) — the node receiving the bound value. Use `statetree_state_list` / `statetree_read` to discover."));
	if (!Params->TryGetStringField(TEXT("target_property"), TargetProperty))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'target_property' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `target_property` (string) — dotted path of the property to bind to (e.g. \"Range.Min\"). Use `statetree_binding_list_bindable` to discover bindable properties."));

	FGuid TargetGuid;
	if (!FGuid::Parse(TargetNodeId, TargetGuid))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid target_node_id GUID"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`target_node_id` must be a parseable FGuid string (e.g. \"12345678-1234-1234-1234-123456789012\")."));

	FPropertyBindingPath TargetPath;
	TargetPath.SetStructID(TargetGuid);
	TArray<FString> Segments;
	TargetProperty.ParseIntoArray(Segments, TEXT("."));
	for (const FString& Segment : Segments)
	{
		if (Segment.Len() >= NAME_SIZE)
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("target_property path segment too long for FName"),
				EMCPErrorCode::InvalidArgument,
				TEXT("Each dotted path segment of `target_property` must be shorter than 1024 characters."));
		TargetPath.AddPathSegment(FName(*Segment));
	}

	EditorData->Modify();
	EditorData->EditorBindings.RemoveBindings(TargetPath);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ---- ListPropertyBindings ----

TSharedPtr<FJsonObject> FStateTreeBindingMgr::ListPropertyBindings(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	const FStateTreeEditorPropertyBindings& Bindings = EditorData->EditorBindings;

	TArray<TSharedPtr<FJsonValue>> BindingsArray;

	for (const FStateTreePropertyPathBinding& Binding : Bindings.GetBindings())
	{
		TSharedPtr<FJsonObject> BindObj = MakeShared<FJsonObject>();

		// Source
		BindObj->SetStringField(TEXT("source_node_id"), Binding.GetSourcePath().GetStructID().ToString());
		BindObj->SetStringField(TEXT("source_property"), Binding.GetSourcePath().ToString());

		// Target
		BindObj->SetStringField(TEXT("target_node_id"), Binding.GetTargetPath().GetStructID().ToString());
		BindObj->SetStringField(TEXT("target_property"), Binding.GetTargetPath().ToString());

		BindingsArray.Add(MakeShared<FJsonValueObject>(BindObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetField(TEXT("bindings"), MakeShared<FJsonValueArray>(BindingsArray));
	Result->SetNumberField(TEXT("count"), BindingsArray.Num());
	return Result;
}

// ---- ListBindableProperties ----

TSharedPtr<FJsonObject> FStateTreeBindingMgr::ListBindableProperties(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStateTree* ST = FStateTreeTypeCache::LoadStateTreeFromParams(Params, Error);
	if (!ST) return Error;
	UStateTreeEditorData* EditorData = FStateTreeTypeCache::GetEditorData(ST, Error);
	if (!EditorData) return Error;

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'node_id' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `node_id` (string GUID) — the node to enumerate bindable properties for."));

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeId, NodeGuid))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Invalid node_id GUID"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`node_id` must be a parseable FGuid string."));

	auto SearchResult = FStateTreeTypeCache::FindNodeByGuid(EditorData, NodeGuid);
	if (!SearchResult.Node)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Node not found: %s"), *NodeId),
			EMCPErrorCode::NodeNotFound,
			TEXT("Node GUID didn't match any node in the StateTree. Use `statetree_state_list` / `statetree_read` to discover GUIDs."));

	TArray<TSharedPtr<FJsonValue>> PropsArray;

	// List properties on the node's instance data
	if (SearchResult.Node->Instance.IsValid())
	{
		const UScriptStruct* Struct = SearchResult.Node->Instance.GetScriptStruct();
		for (TFieldIterator<FProperty> PropIt(Struct); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

			TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
			PropObj->SetStringField(TEXT("name"), Prop->GetName());
			PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());

			// Determine usage from metadata
			FString Usage = TEXT("input"); // default: bindable as target
			if (Prop->HasMetaData(TEXT("Output")))
				Usage = TEXT("output");
			PropObj->SetStringField(TEXT("usage"), Usage);
			PropObj->SetStringField(TEXT("node_id"), NodeId);

			if (!Prop->GetMetaData(TEXT("Category")).IsEmpty())
				PropObj->SetStringField(TEXT("category"), Prop->GetMetaData(TEXT("Category")));

			PropsArray.Add(MakeShared<FJsonValueObject>(PropObj));
		}
	}

	// Also list available binding sources: evaluator outputs, global task outputs
	TArray<TSharedPtr<FJsonValue>> SourcesArray;

	for (const FStateTreeEditorNode& EvalNode : EditorData->Evaluators)
	{
		if (!EvalNode.Instance.IsValid()) continue;
		const UScriptStruct* Struct = EvalNode.Instance.GetScriptStruct();
		for (TFieldIterator<FProperty> PropIt(Struct); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

			TSharedPtr<FJsonObject> SrcObj = MakeShared<FJsonObject>();
			SrcObj->SetStringField(TEXT("source_node_id"), EvalNode.ID.ToString());
			SrcObj->SetStringField(TEXT("source_node_type"), EvalNode.Node.IsValid() ? EvalNode.Node.GetScriptStruct()->GetName() : TEXT("Unknown"));
			SrcObj->SetStringField(TEXT("property"), Prop->GetName());
			SrcObj->SetStringField(TEXT("type"), Prop->GetCPPType());
			SourcesArray.Add(MakeShared<FJsonValueObject>(SrcObj));
		}
	}

	for (const FStateTreeEditorNode& GlobalTask : EditorData->GlobalTasks)
	{
		if (!GlobalTask.Instance.IsValid()) continue;
		const UScriptStruct* Struct = GlobalTask.Instance.GetScriptStruct();
		for (TFieldIterator<FProperty> PropIt(Struct); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

			TSharedPtr<FJsonObject> SrcObj = MakeShared<FJsonObject>();
			SrcObj->SetStringField(TEXT("source_node_id"), GlobalTask.ID.ToString());
			SrcObj->SetStringField(TEXT("source_node_type"), GlobalTask.Node.IsValid() ? GlobalTask.Node.GetScriptStruct()->GetName() : TEXT("Unknown"));
			SrcObj->SetStringField(TEXT("property"), Prop->GetName());
			SrcObj->SetStringField(TEXT("type"), Prop->GetCPPType());
			SourcesArray.Add(MakeShared<FJsonValueObject>(SrcObj));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetField(TEXT("node_properties"), MakeShared<FJsonValueArray>(PropsArray));
	Result->SetField(TEXT("available_sources"), MakeShared<FJsonValueArray>(SourcesArray));
	return Result;
}
