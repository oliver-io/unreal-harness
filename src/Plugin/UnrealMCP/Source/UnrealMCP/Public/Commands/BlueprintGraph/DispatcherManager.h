// Blueprint event-dispatcher (multicast delegate) authoring.
//
// Mirrors the engine's own authoring path FBlueprintEditor::OnAddNewDelegate
// (UE 5.7, Editor/Kismet/Private/BlueprintEditor.cpp:9606) — a real dispatcher
// is a PC_MCDelegate member variable + a delegate SIGNATURE GRAPH registered in
// UBlueprint::DelegateSignatureGraphs, NOT a plain MulticastDelegate variable.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;

/**
 * Utility class for authoring Blueprint event dispatchers (multicast delegates).
 */
class UNREALMCP_API FDispatcherManager
{
public:
    /**
     * Creates a new event dispatcher on a Blueprint.
     * @param Params JSON: blueprint_name, dispatcher_name, optional params[] ({name,type,is_array}), dry_run
     * @return JSON with success, dispatcher_name, graph_id, params_added
     */
    static TSharedPtr<FJsonObject> CreateDispatcher(const TSharedPtr<FJsonObject>& Params);
};
