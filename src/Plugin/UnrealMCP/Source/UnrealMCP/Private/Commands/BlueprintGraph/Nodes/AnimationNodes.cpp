#include "Commands/BlueprintGraph/Nodes/AnimationNodes.h"
#include "Commands/MCPCommonUtils.h"
#include "Commands/BlueprintGraph/Nodes/NodeCreatorUtils.h"
#include "K2Node_Timeline.h"
#include "Json.h"

UK2Node* FAnimationNodeCreator::CreateTimelineNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph || !Params.IsValid())
	{
		return nullptr;
	}

	UK2Node_Timeline* TimelineNode = NewObject<UK2Node_Timeline>(Graph);
	if (!TimelineNode)
	{
		return nullptr;
	}

	// Set timeline name if provided BEFORE initialization
	FString TimelineName;
	if (Params->TryGetStringField(TEXT("timeline_name"), TimelineName))
	{
		if (TimelineName.Len() >= NAME_SIZE)
		{
			UE_LOG(LogUnrealMCP, Warning, TEXT("CreateTimelineNode: timeline_name length %d exceeds max FName length %d; aborting."), TimelineName.Len(), NAME_SIZE);
			return nullptr;
		}
		TimelineNode->TimelineName = FName(*TimelineName);
	}

	double PosX, PosY;
	FNodeCreatorUtils::ExtractNodePosition(Params, PosX, PosY);
	TimelineNode->NodePosX = static_cast<int32>(PosX);
	TimelineNode->NodePosY = static_cast<int32>(PosY);

	Graph->AddNode(TimelineNode, true, false);
	FNodeCreatorUtils::InitializeK2Node(TimelineNode, Graph);

	return TimelineNode;
}
