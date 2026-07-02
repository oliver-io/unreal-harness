// PCG (Procedural Content Generation) command handlers.
//
// Vertical slice surfacing PCG in the MCP — discovery, read, graph authoring,
// and driving a component to generate. Design notes + the full proposed surface
// live in docs/proposals/pcg-mcp.md.
//
// All authoring drives the runtime UPCGGraph API directly (AddNodeOfType /
// AddEdge); the editor graph mirror reconstructs itself off NotifyGraphChanged.
// Editor-only entry points (NotifyGraphChanged, SetNodeTitle, SetNodePosition,
// GetDefaultNodeTitle, GetType, bExposeToLibrary) are guarded with WITH_EDITOR —
// the UnrealMCP module is editor-only, so these are always available at runtime
// here, but the guards keep the file honest and portable.

#include "Commands/MCPPCGCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGEdge.h"
#include "PCGSettings.h"
#include "PCGComponent.h"
#include "PCGCommon.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

// ── Local helpers ───────────────────────────────────────────────────────────

namespace
{
	// Resolve the "graph_path" param to a loaded UPCGGraph, or fill OutError.
	UPCGGraph* LoadGraphFromParam(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutError)
	{
		FString GraphPath;
		if (!Params->TryGetStringField(TEXT("graph_path"), GraphPath) || GraphPath.IsEmpty())
		{
			OutError = FMCPCommonUtils::CreateErrorResponse(
				TEXT("Missing 'graph_path'"),
				EMCPErrorCode::InvalidArgument,
				TEXT("Pass `graph_path` (string) — the /Game/... path of a PCG Graph asset. Use pcg_list_graphs to find one or pcg_graph_create to make one."));
			return nullptr;
		}

		UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
		if (!Graph)
		{
			OutError = FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("PCG Graph not found: %s"), *GraphPath),
				EMCPErrorCode::AssetNotFound,
				TEXT("No UPCGGraph at that path. Pass a full object path like /Game/PCG/PCG_Scatter. Use pcg_list_graphs to enumerate."));
			return nullptr;
		}
		return Graph;
	}

	// Friendly EPCGSettingsType → string (editor-only enum on UPCGSettings::GetType()).
	FString SettingsTypeToString(EPCGSettingsType Type)
	{
		switch (Type)
		{
		case EPCGSettingsType::InputOutput:            return TEXT("InputOutput");
		case EPCGSettingsType::Spatial:                return TEXT("Spatial");
		case EPCGSettingsType::Density:                return TEXT("Density");
		case EPCGSettingsType::Blueprint:              return TEXT("Blueprint");
		case EPCGSettingsType::Metadata:               return TEXT("Metadata");
		case EPCGSettingsType::Filter:                 return TEXT("Filter");
		case EPCGSettingsType::Sampler:                return TEXT("Sampler");
		case EPCGSettingsType::Spawner:                return TEXT("Spawner");
		case EPCGSettingsType::Subgraph:               return TEXT("Subgraph");
		case EPCGSettingsType::Debug:                  return TEXT("Debug");
		case EPCGSettingsType::Generic:                return TEXT("Generic");
		case EPCGSettingsType::Param:                  return TEXT("Param");
		case EPCGSettingsType::HierarchicalGeneration: return TEXT("HierarchicalGeneration");
		case EPCGSettingsType::ControlFlow:            return TEXT("ControlFlow");
		case EPCGSettingsType::PointOps:               return TEXT("PointOps");
		case EPCGSettingsType::GraphParameters:        return TEXT("GraphParameters");
		case EPCGSettingsType::Reroute:                return TEXT("Reroute");
		case EPCGSettingsType::GPU:                    return TEXT("GPU");
		case EPCGSettingsType::DynamicMesh:            return TEXT("DynamicMesh");
		case EPCGSettingsType::DataLayers:             return TEXT("DataLayers");
		case EPCGSettingsType::Resource:               return TEXT("Resource");
		default:                                       return TEXT("Unknown");
		}
	}

	// Resolve a settings class from a string: full class path (/Script/PCG.PCGCreatePointsSettings),
	// or a bare class name (PCGCreatePointsSettings, case-insensitive, optional leading 'U').
	UClass* ResolveSettingsClass(const FString& InName)
	{
		if (InName.IsEmpty())
		{
			return nullptr;
		}

		// Path form.
		if (InName.Contains(TEXT("/")) || InName.Contains(TEXT(".")))
		{
			if (UClass* Loaded = LoadObject<UClass>(nullptr, *InName))
			{
				if (Loaded->IsChildOf(UPCGSettings::StaticClass()))
				{
					return Loaded;
				}
			}
		}

		FString Bare = InName;
		Bare.RemoveFromStart(TEXT("U"));

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class->IsChildOf(UPCGSettings::StaticClass()) || Class == UPCGSettings::StaticClass())
			{
				continue;
			}
			if (Class->HasAnyClassFlags(CLASS_Abstract))
			{
				continue;
			}
			const FString ClassName = Class->GetName();
			if (ClassName.Equals(InName, ESearchCase::IgnoreCase) || ClassName.Equals(Bare, ESearchCase::IgnoreCase))
			{
				return Class;
			}
		}
		return nullptr;
	}

	// Find a node within a graph by: object name (GetName), authored title, or the
	// special tokens "InputNode"/"OutputNode" (case-insensitive).
	UPCGNode* FindNode(UPCGGraph* Graph, const FString& Id)
	{
		if (!Graph || Id.IsEmpty())
		{
			return nullptr;
		}
		if (Id.Equals(TEXT("InputNode"), ESearchCase::IgnoreCase) || Id.Equals(TEXT("Input"), ESearchCase::IgnoreCase))
		{
			return Graph->GetInputNode();
		}
		if (Id.Equals(TEXT("OutputNode"), ESearchCase::IgnoreCase) || Id.Equals(TEXT("Output"), ESearchCase::IgnoreCase))
		{
			return Graph->GetOutputNode();
		}

		for (UPCGNode* Node : Graph->GetNodes())
		{
			if (!Node)
			{
				continue;
			}
			if (Node->GetName().Equals(Id, ESearchCase::IgnoreCase))
			{
				return Node;
			}
			if (Node->GetAuthoredTitleName() != NAME_None && Node->GetAuthoredTitleName().ToString().Equals(Id, ESearchCase::IgnoreCase))
			{
				return Node;
			}
		}
		return nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> PinLabels(const TArray<TObjectPtr<UPCGPin>>& Pins)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const UPCGPin* Pin : Pins)
		{
			if (Pin)
			{
				Out.Add(MakeShared<FJsonValueString>(Pin->Properties.Label.ToString()));
			}
		}
		return Out;
	}

	// Pin labels straight from a settings' declared pin properties (used for the
	// node-type palette, where there is no instantiated node yet).
	TArray<TSharedPtr<FJsonValue>> PinLabels_FromProperties(const TArray<FPCGPinProperties>& Props)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FPCGPinProperties& P : Props)
		{
			Out.Add(MakeShared<FJsonValueString>(P.Label.ToString()));
		}
		return Out;
	}

	TSharedPtr<FJsonObject> NodeToJson(UPCGNode* Node)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Node->GetName());
		UPCGSettings* Settings = Node->GetSettings();
		Obj->SetStringField(TEXT("settings_class"), Settings ? Settings->GetClass()->GetName() : TEXT("None"));
#if WITH_EDITOR
		Obj->SetStringField(TEXT("title"), Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
		int32 PosX = 0, PosY = 0;
		Node->GetNodePosition(PosX, PosY);
		Obj->SetNumberField(TEXT("pos_x"), PosX);
		Obj->SetNumberField(TEXT("pos_y"), PosY);
#endif
		Obj->SetArrayField(TEXT("input_pins"), PinLabels(Node->GetInputPins()));
		Obj->SetArrayField(TEXT("output_pins"), PinLabels(Node->GetOutputPins()));
		return Obj;
	}

	// Find an actor in the editor world by label or object name.
	AActor* FindEditorActor(const FString& Name)
	{
		if (!GEditor)
		{
			return nullptr;
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			if (Actor->GetActorLabel().Equals(Name, ESearchCase::IgnoreCase) || Actor->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
		}
		return nullptr;
	}

	void SaveGraph(UPCGGraph* Graph)
	{
		if (Graph && Graph->GetPackage())
		{
			Graph->MarkPackageDirty();
			UEditorAssetLibrary::SaveAsset(Graph->GetPackage()->GetName(), /*bOnlyIfIsDirty*/false);
		}
	}
}

// ── dispatch ────────────────────────────────────────────────────────────────

FMCPPCGCommands::FMCPPCGCommands() {}

TSharedPtr<FJsonObject> FMCPPCGCommands::HandleCommand(
	const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("pcg_list_graphs"))        return HandleListGraphs(Params);
	if (CommandType == TEXT("pcg_list_node_types"))    return HandleListNodeTypes(Params);
	if (CommandType == TEXT("pcg_graph_read"))         return HandleGraphRead(Params);
	if (CommandType == TEXT("pcg_graph_create"))       return HandleGraphCreate(Params);
	if (CommandType == TEXT("pcg_node_add"))           return HandleNodeAdd(Params);
	if (CommandType == TEXT("pcg_node_connect"))       return HandleNodeConnect(Params);
	if (CommandType == TEXT("pcg_node_set_property"))  return HandleNodeSetProperty(Params);
	if (CommandType == TEXT("pcg_component_add"))      return HandleComponentAdd(Params);
	if (CommandType == TEXT("pcg_component_generate")) return HandleComponentGenerate(Params);

	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown PCG command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("Valid PCG commands: pcg_list_graphs, pcg_list_node_types, pcg_graph_read, pcg_graph_create, pcg_node_add, pcg_node_connect, pcg_node_set_property, pcg_component_add, pcg_component_generate."));
}

// ── pcg_list_graphs ──────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPPCGCommands::HandleListGraphs(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter;
	Params->TryGetStringField(TEXT("path_filter"), PathFilter);
	if (PathFilter.IsEmpty()) PathFilter = TEXT("/Game");

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	TArray<FAssetData> Assets;
	ARM.Get().GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/PCG"), TEXT("PCGGraph")), Assets);
	ARM.Get().GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/PCG"), TEXT("PCGGraphInstance")), Assets);

	TArray<TSharedPtr<FJsonValue>> GraphArray;
	for (const FAssetData& Asset : Assets)
	{
		FString Path = Asset.GetObjectPathString();
		if (!Path.StartsWith(PathFilter)) continue;

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Obj->SetStringField(TEXT("path"), Path);
		Obj->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
		GraphArray.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("graphs"), GraphArray);
	Result->SetNumberField(TEXT("count"), GraphArray.Num());
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── pcg_list_node_types ──────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPPCGCommands::HandleListNodeTypes(const TSharedPtr<FJsonObject>& Params)
{
	FString NameFilter, CategoryFilter;
	Params->TryGetStringField(TEXT("name_filter"), NameFilter);
	Params->TryGetStringField(TEXT("category_filter"), CategoryFilter);

	TArray<TSharedPtr<FJsonValue>> TypeArray;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class->IsChildOf(UPCGSettings::StaticClass()) || Class == UPCGSettings::StaticClass())
		{
			continue;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Hidden))
		{
			continue;
		}

		const UPCGSettings* CDO = Class->GetDefaultObject<UPCGSettings>();
		if (!CDO)
		{
			continue;
		}

#if WITH_EDITOR
		if (!CDO->bExposeToLibrary)
		{
			continue;
		}
		const FString Title = CDO->GetDefaultNodeTitle().ToString();
		const FString TypeStr = SettingsTypeToString(CDO->GetType());
		const int32 PreconfiguredCount = CDO->GetPreconfiguredInfo().Num();
#else
		const FString Title = Class->GetName();
		const FString TypeStr = TEXT("Unknown");
		const int32 PreconfiguredCount = 0;
#endif

		const FString ClassName = Class->GetName();
		if (!NameFilter.IsEmpty() && !ClassName.Contains(NameFilter) && !Title.Contains(NameFilter))
		{
			continue;
		}
		if (!CategoryFilter.IsEmpty() && !TypeStr.Contains(CategoryFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("class_name"), ClassName);
		Obj->SetStringField(TEXT("class_path"), Class->GetPathName());
		Obj->SetStringField(TEXT("title"), Title);
		Obj->SetStringField(TEXT("type"), TypeStr);
		Obj->SetNumberField(TEXT("preconfigured_count"), PreconfiguredCount);
		Obj->SetArrayField(TEXT("input_pins"), PinLabels_FromProperties(CDO->InputPinProperties()));
		Obj->SetArrayField(TEXT("output_pins"), PinLabels_FromProperties(CDO->OutputPinProperties()));
		TypeArray.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("node_types"), TypeArray);
	Result->SetNumberField(TEXT("count"), TypeArray.Num());
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── pcg_graph_read ───────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPPCGCommands::HandleGraphRead(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UPCGGraph* Graph = LoadGraphFromParam(Params, Error);
	if (!Graph) return Error;

	// Nodes — include the input/output nodes plus all interior nodes.
	TArray<TSharedPtr<FJsonValue>> NodeArray;
	TArray<UPCGNode*> AllNodes;
	if (UPCGNode* In = Graph->GetInputNode())  { AllNodes.Add(In); }
	if (UPCGNode* Out = Graph->GetOutputNode()) { AllNodes.Add(Out); }
	AllNodes.Append(Graph->GetNodes());

	for (UPCGNode* Node : AllNodes)
	{
		if (Node) { NodeArray.Add(MakeShared<FJsonValueObject>(NodeToJson(Node))); }
	}

	// Edges — walk every output pin's edges.
	TArray<TSharedPtr<FJsonValue>> EdgeArray;
	for (UPCGNode* Node : AllNodes)
	{
		if (!Node) continue;
		for (const UPCGPin* OutPin : Node->GetOutputPins())
		{
			if (!OutPin) continue;
			for (const UPCGEdge* Edge : OutPin->Edges)
			{
				if (!Edge || !Edge->InputPin || !Edge->OutputPin) continue;
				// On an edge, InputPin is the upstream (output) side and OutputPin
				// is the downstream (input) side. Only emit from the upstream node.
				const UPCGPin* UpstreamPin = Edge->InputPin;
				const UPCGPin* DownstreamPin = Edge->OutputPin;
				if (UpstreamPin != OutPin) continue;
				if (!UpstreamPin->Node || !DownstreamPin->Node) continue;

				TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
				EdgeObj->SetStringField(TEXT("from_node"), UpstreamPin->Node->GetName());
				EdgeObj->SetStringField(TEXT("from_pin"), UpstreamPin->Properties.Label.ToString());
				EdgeObj->SetStringField(TEXT("to_node"), DownstreamPin->Node->GetName());
				EdgeObj->SetStringField(TEXT("to_pin"), DownstreamPin->Properties.Label.ToString());
				EdgeArray.Add(MakeShared<FJsonValueObject>(EdgeObj));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("graph"), Graph->GetPathName());
	Result->SetStringField(TEXT("input_node"), Graph->GetInputNode() ? Graph->GetInputNode()->GetName() : TEXT(""));
	Result->SetStringField(TEXT("output_node"), Graph->GetOutputNode() ? Graph->GetOutputNode()->GetName() : TEXT(""));
	Result->SetArrayField(TEXT("nodes"), NodeArray);
	Result->SetArrayField(TEXT("edges"), EdgeArray);
	Result->SetNumberField(TEXT("node_count"), NodeArray.Num());
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── pcg_graph_create ─────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPPCGCommands::HandleGraphCreate(const TSharedPtr<FJsonObject>& Params)
{
	FString GraphPath;
	if (!Params->TryGetStringField(TEXT("graph_path"), GraphPath) || GraphPath.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'graph_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `graph_path` (string) — the full /Game/... destination for the new PCG Graph, e.g. /Game/PCG/PCG_Scatter."));
	}

	int32 LastSlash = INDEX_NONE;
	if (!GraphPath.FindLastChar(TEXT('/'), LastSlash) || LastSlash <= 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Malformed graph_path: %s"), *GraphPath),
			EMCPErrorCode::InvalidArgument,
			TEXT("`graph_path` must be a full content path like /Game/PCG/PCG_Scatter."));
	}
	const FString PackagePath = GraphPath.Left(LastSlash);
	FString AssetName = GraphPath.Mid(LastSlash + 1);
	{
		FString NameOnly;
		if (AssetName.Split(TEXT("."), &NameOnly, nullptr)) { AssetName = NameOnly; }
	}
	const FString FullPackageName = PackagePath / AssetName;

	if (UEditorAssetLibrary::DoesAssetExist(FullPackageName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Asset already exists: %s"), *FullPackageName),
			EMCPErrorCode::NameCollision,
			TEXT("No silent overwrite. Pick a different graph_path or delete the existing asset first."));
	}

	UPackage* Package = CreatePackage(*FullPackageName);
	if (!Package)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Could not create package: %s"), *FullPackageName),
			EMCPErrorCode::Internal,
			TEXT("CreatePackage returned null — verify the /Game/... path is writable."));
	}

	UPCGGraph* Graph = NewObject<UPCGGraph>(
		Package, UPCGGraph::StaticClass(), FName(*AssetName),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!Graph)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("NewObject<UPCGGraph> returned null"),
			EMCPErrorCode::Internal,
			TEXT("Engine refused to construct the graph; check the editor log."));
	}

	FAssetRegistryModule::AssetCreated(Graph);
	Graph->MarkPackageDirty();
	if (!UEditorAssetLibrary::SaveAsset(FullPackageName, /*bOnlyIfIsDirty*/false))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Graph created in memory but failed to save: %s"), *FullPackageName),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::SaveAsset returned false — SaveAsset bails while a PIE session is active. Stop PIE and retry."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("graph"), Graph->GetPathName());
	Result->SetStringField(TEXT("asset_path"), FullPackageName);
	Result->SetStringField(TEXT("input_node"), Graph->GetInputNode() ? Graph->GetInputNode()->GetName() : TEXT(""));
	Result->SetStringField(TEXT("output_node"), Graph->GetOutputNode() ? Graph->GetOutputNode()->GetName() : TEXT(""));
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── pcg_node_add ─────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPPCGCommands::HandleNodeAdd(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UPCGGraph* Graph = LoadGraphFromParam(Params, Error);
	if (!Graph) return Error;

	FString SettingsClassName;
	if (!Params->TryGetStringField(TEXT("settings_class"), SettingsClassName) || SettingsClassName.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'settings_class'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `settings_class` (string) — a PCG settings class name (e.g. PCGCreatePointsSettings) or its class_path. Use pcg_list_node_types to discover valid values."));
	}

	UClass* SettingsClass = ResolveSettingsClass(SettingsClassName);
	if (!SettingsClass)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Could not resolve PCG settings class: %s"), *SettingsClassName),
			EMCPErrorCode::ClassNotLoaded,
			TEXT("Pass a class name or class_path from pcg_list_node_types (e.g. PCGStaticMeshSpawnerSettings or /Script/PCG.PCGStaticMeshSpawnerSettings)."));
	}

	Graph->Modify();
	UPCGSettings* OutSettings = nullptr;
	UPCGNode* Node = Graph->AddNodeOfType(SettingsClass, OutSettings);
	if (!Node)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("AddNodeOfType failed for: %s"), *SettingsClass->GetName()),
			EMCPErrorCode::Internal,
			TEXT("The graph refused to add a node of that settings class; check the editor log."));
	}

#if WITH_EDITOR
	FString NodeTitle;
	if (Params->TryGetStringField(TEXT("node_title"), NodeTitle) && !NodeTitle.IsEmpty())
	{
		Node->SetNodeTitle(FName(*NodeTitle));
	}

	double PosX = 0.0, PosY = 0.0;
	const bool bHasX = Params->TryGetNumberField(TEXT("pos_x"), PosX);
	const bool bHasY = Params->TryGetNumberField(TEXT("pos_y"), PosY);
	if (bHasX || bHasY)
	{
		Node->SetNodePosition(static_cast<int32>(PosX), static_cast<int32>(PosY));
	}
	// AddNodeOfType already fires the graph's (private) change notification
	// internally; no explicit NotifyGraphChanged needed here.
#endif

	SaveGraph(Graph);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("node"), NodeToJson(Node));
	Result->SetStringField(TEXT("graph"), Graph->GetPathName());
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── pcg_node_connect ─────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPPCGCommands::HandleNodeConnect(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UPCGGraph* Graph = LoadGraphFromParam(Params, Error);
	if (!Graph) return Error;

	FString FromId, ToId;
	if (!Params->TryGetStringField(TEXT("from_node"), FromId) || !Params->TryGetStringField(TEXT("to_node"), ToId))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'from_node' and/or 'to_node'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `from_node` and `to_node` (node ids from pcg_graph_read, or the tokens 'InputNode'/'OutputNode')."));
	}

	UPCGNode* FromNode = FindNode(Graph, FromId);
	UPCGNode* ToNode = FindNode(Graph, ToId);
	if (!FromNode || !ToNode)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Node not found: %s"), !FromNode ? *FromId : *ToId),
			EMCPErrorCode::NodeNotFound,
			TEXT("Use pcg_graph_read to list node ids. 'InputNode'/'OutputNode' address the graph's input/output."));
	}

	// Default to each node's first output/input pin when not specified.
	FString FromPin, ToPin;
	Params->TryGetStringField(TEXT("from_pin"), FromPin);
	Params->TryGetStringField(TEXT("to_pin"), ToPin);
	if (FromPin.IsEmpty())
	{
		if (FromNode->GetOutputPins().Num() == 0)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("from_node '%s' has no output pins"), *FromId),
				EMCPErrorCode::PinNotFound,
				TEXT("That node produces no output. Pick a different from_node."));
		}
		FromPin = FromNode->GetOutputPins()[0]->Properties.Label.ToString();
	}
	if (ToPin.IsEmpty())
	{
		if (ToNode->GetInputPins().Num() == 0)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("to_node '%s' has no input pins"), *ToId),
				EMCPErrorCode::PinNotFound,
				TEXT("That node accepts no input. Pick a different to_node."));
		}
		ToPin = ToNode->GetInputPins()[0]->Properties.Label.ToString();
	}

	Graph->Modify();
	UPCGNode* Result0 = Graph->AddEdge(FromNode, FName(*FromPin), ToNode, FName(*ToPin));
	if (!Result0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("AddEdge failed: %s.%s -> %s.%s"), *FromId, *FromPin, *ToId, *ToPin),
			EMCPErrorCode::InvalidArgument,
			TEXT("AddEdge returned null — the pin labels may be wrong or the pin types incompatible. Use pcg_graph_read / pcg_list_node_types to confirm pin labels."));
	}

	// AddEdge fires the graph's internal change notification itself.
	SaveGraph(Graph);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("from_node"), FromNode->GetName());
	Result->SetStringField(TEXT("from_pin"), FromPin);
	Result->SetStringField(TEXT("to_node"), ToNode->GetName());
	Result->SetStringField(TEXT("to_pin"), ToPin);
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── pcg_node_set_property ────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPPCGCommands::HandleNodeSetProperty(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UPCGGraph* Graph = LoadGraphFromParam(Params, Error);
	if (!Graph) return Error;

	FString NodeId, PropertyName;
	if (!Params->TryGetStringField(TEXT("node"), NodeId) || !Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'node' and/or 'property_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `node` (node id from pcg_graph_read), `property_name`, and `property_value`."));
	}

	const TSharedPtr<FJsonValue> PropertyValue = Params->TryGetField(TEXT("property_value"));
	if (!PropertyValue.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'property_value'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `property_value` — the value to set (string/number/bool/object as appropriate for the property)."));
	}

	UPCGNode* Node = FindNode(Graph, NodeId);
	if (!Node)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Node not found: %s"), *NodeId),
			EMCPErrorCode::NodeNotFound,
			TEXT("Use pcg_graph_read to list node ids."));
	}

	UPCGSettings* Settings = Node->GetSettings();
	if (!Settings)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Node '%s' has no settings object"), *NodeId),
			EMCPErrorCode::Internal,
			TEXT("This node exposes no editable settings."));
	}

	Settings->Modify();
	FString PropError;
	if (!FMCPCommonUtils::SetObjectProperty(Settings, PropertyName, PropertyValue, PropError))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to set '%s' on %s: %s"), *PropertyName, *Settings->GetClass()->GetName(), *PropError),
			EMCPErrorCode::InvalidArgument,
			TEXT("Check the property name/type. Reflectable EditAnywhere properties only."));
	}

#if WITH_EDITOR
	// PostEditChange broadcasts the settings-changed delegate, which propagates
	// the change into the owning graph (its NotifyGraphChanged is private).
	Settings->PostEditChange();
#endif
	SaveGraph(Graph);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node"), Node->GetName());
	Result->SetStringField(TEXT("property_name"), PropertyName);
	Result->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetName());
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── pcg_component_add ────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPPCGCommands::HandleComponentAdd(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'actor_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `actor_name` — the label or name of an actor in the level. Spawn one first with actor_spawn if needed."));
	}

	AActor* Actor = FindEditorActor(ActorName);
	if (!Actor)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Actor not found: %s"), *ActorName),
			EMCPErrorCode::ActorNotFound,
			TEXT("No actor with that label/name in the editor world. Use find_actors_by_name or actor_query to confirm."));
	}

	// Optional graph to assign.
	UPCGGraph* Graph = nullptr;
	FString GraphPath;
	if (Params->TryGetStringField(TEXT("graph_path"), GraphPath) && !GraphPath.IsEmpty())
	{
		Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
		if (!Graph)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("PCG Graph not found: %s"), *GraphPath),
				EMCPErrorCode::AssetNotFound,
				TEXT("graph_path did not resolve to a UPCGGraph. Omit it to add an unconfigured component, or pass a valid path."));
		}
	}

	UPCGComponent* Comp = NewObject<UPCGComponent>(Actor, UPCGComponent::StaticClass(), NAME_None, RF_Transactional);
	if (!Comp)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("NewObject<UPCGComponent> returned null"),
			EMCPErrorCode::Internal,
			TEXT("Engine refused to construct the component; check the editor log."));
	}

	Actor->Modify();
	Actor->AddInstanceComponent(Comp);
	Comp->RegisterComponent();
	if (Graph)
	{
		Comp->SetGraphLocal(Graph);
	}
	Actor->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("component"), Comp->GetName());
	Result->SetStringField(TEXT("graph"), Graph ? Graph->GetPathName() : TEXT(""));
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ── pcg_component_generate ───────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPPCGCommands::HandleComponentGenerate(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'actor_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass `actor_name` — the actor carrying the PCG component to generate."));
	}

	AActor* Actor = FindEditorActor(ActorName);
	if (!Actor)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Actor not found: %s"), *ActorName),
			EMCPErrorCode::ActorNotFound,
			TEXT("No actor with that label/name in the editor world."));
	}

	UPCGComponent* Comp = Actor->FindComponentByClass<UPCGComponent>();
	if (!Comp)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Actor '%s' has no UPCGComponent"), *ActorName),
			EMCPErrorCode::ActorNotFound,
			TEXT("Add one first with pcg_component_add."));
	}

	if (!Comp->GetGraph())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("PCG component on '%s' has no graph assigned"), *ActorName),
			EMCPErrorCode::InvalidArgument,
			TEXT("Assign a graph via pcg_component_add (graph_path) before generating."));
	}

	bool bForce = true;
	Params->TryGetBoolField(TEXT("force"), bForce);

	Comp->GenerateLocal(bForce);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("component"), Comp->GetName());
	Result->SetBoolField(TEXT("generation_requested"), true);
	Result->SetBoolField(TEXT("is_generating"), Comp->IsGenerating());
	Result->SetStringField(TEXT("note"), TEXT("PCG generation is asynchronous (scheduled on the PCG subsystem); poll the level or screenshot after a moment to see results."));
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}
