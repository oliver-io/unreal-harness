#include "Commands/BlueprintGraph/Nodes/SpecializedNodes.h"
#include "Commands/BlueprintGraph/Nodes/NodeCreatorUtils.h"
#include "K2Node_GetDataTableRow.h"
#include "K2Node_AddComponentByClass.h"
#include "K2Node_Self.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "K2Node_GenericCreateObject.h"
#include "K2Node_Knot.h"
#include "EdGraphSchema_K2.h"
#include "Commands/MCPCommonUtils.h"
#include "Json.h"

UK2Node* FSpecializedNodeCreator::CreateGetDataTableRowNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph || !Params.IsValid())
	{
		return nullptr;
	}

	UK2Node_GetDataTableRow* DataTableRowNode = NewObject<UK2Node_GetDataTableRow>(Graph);
	if (!DataTableRowNode)
	{
		return nullptr;
	}

	// Note: DataTable property not available in UE5.5 API
	// Parameter ignored - node created without data table reference

	double PosX, PosY;
	FNodeCreatorUtils::ExtractNodePosition(Params, PosX, PosY);
	DataTableRowNode->NodePosX = static_cast<int32>(PosX);
	DataTableRowNode->NodePosY = static_cast<int32>(PosY);

	Graph->AddNode(DataTableRowNode, true, false);
	FNodeCreatorUtils::InitializeK2Node(DataTableRowNode, Graph);

	return DataTableRowNode;
}

UK2Node* FSpecializedNodeCreator::CreateAddComponentByClassNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph || !Params.IsValid())
	{
		return nullptr;
	}

	UK2Node_AddComponentByClass* AddComponentNode = NewObject<UK2Node_AddComponentByClass>(Graph);
	if (!AddComponentNode)
	{
		return nullptr;
	}

	double PosX, PosY;
	FNodeCreatorUtils::ExtractNodePosition(Params, PosX, PosY);
	AddComponentNode->NodePosX = static_cast<int32>(PosX);
	AddComponentNode->NodePosY = static_cast<int32>(PosY);

	Graph->AddNode(AddComponentNode, true, false);
	FNodeCreatorUtils::InitializeK2Node(AddComponentNode, Graph);

	return AddComponentNode;
}

UK2Node* FSpecializedNodeCreator::CreateSelfNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph || !Params.IsValid())
	{
		return nullptr;
	}

	UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Graph);
	if (!SelfNode)
	{
		return nullptr;
	}

	double PosX, PosY;
	FNodeCreatorUtils::ExtractNodePosition(Params, PosX, PosY);
	SelfNode->NodePosX = static_cast<int32>(PosX);
	SelfNode->NodePosY = static_cast<int32>(PosY);

	Graph->AddNode(SelfNode, true, false);
	FNodeCreatorUtils::InitializeK2Node(SelfNode, Graph);

	return SelfNode;
}

UK2Node* FSpecializedNodeCreator::CreateConstructObjectNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph || !Params.IsValid())
	{
		return nullptr;
	}

	// GAP-038: UK2Node_ConstructObjectFromClass is ABSTRACT — instantiating it produced
	// a node that broke compilation ("Unexpected node type K2Node_ConstructObjectFromClass
	// encountered"). Use the concrete UK2Node_GenericCreateObject, which is the
	// general-purpose construct-object node and compiles for any UObject class.
	UK2Node_GenericCreateObject* ConstructObjNode = NewObject<UK2Node_GenericCreateObject>(Graph);
	if (!ConstructObjNode)
	{
		return nullptr;
	}

	double PosX, PosY;
	FNodeCreatorUtils::ExtractNodePosition(Params, PosX, PosY);
	ConstructObjNode->NodePosX = static_cast<int32>(PosX);
	ConstructObjNode->NodePosY = static_cast<int32>(PosY);

	Graph->AddNode(ConstructObjNode, true, false);
	FNodeCreatorUtils::InitializeK2Node(ConstructObjNode, Graph);

	// Resolve the class pin at creation if a class was supplied (accepted under any of
	// the common keys). Setting the Class pin's default object conforms the node's
	// typed Return Value output so it can be wired without a cast.
	FString ClassStr;
	if (Params->TryGetStringField(TEXT("target_class"), ClassStr) ||
	    Params->TryGetStringField(TEXT("class"), ClassStr) ||
	    Params->TryGetStringField(TEXT("class_name"), ClassStr))
	{
		if (!ClassStr.IsEmpty())
		{
			if (UClass* ResolvedClass = FMCPCommonUtils::ResolveClass(ClassStr))
			{
				if (UEdGraphPin* ClassPin = ConstructObjNode->FindPin(TEXT("Class")))
				{
					if (const UEdGraphSchema* Schema = ClassPin->GetSchema())
					{
						Schema->TrySetDefaultObject(*ClassPin, ResolvedClass);
					}
					else
					{
						ClassPin->DefaultObject = ResolvedClass;
					}
					ConstructObjNode->ReconstructNode();
				}
			}
		}
	}

	return ConstructObjNode;
}

UK2Node* FSpecializedNodeCreator::CreateKnotNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph || !Params.IsValid())
	{
		return nullptr;
	}

	UK2Node_Knot* KnotNode = NewObject<UK2Node_Knot>(Graph);
	if (!KnotNode)
	{
		return nullptr;
	}

	double PosX, PosY;
	FNodeCreatorUtils::ExtractNodePosition(Params, PosX, PosY);
	KnotNode->NodePosX = static_cast<int32>(PosX);
	KnotNode->NodePosY = static_cast<int32>(PosY);

	Graph->AddNode(KnotNode, true, false);
	FNodeCreatorUtils::InitializeK2Node(KnotNode, Graph);

	return KnotNode;
}
