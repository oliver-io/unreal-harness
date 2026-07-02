#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class FMCPLogCollector;

/**
 * Handler class for Automation MCP commands.
 * PIE control, screenshot capture, input simulation, console commands.
 */
class UNREALMCP_API FMCPAutomationCommands
{
public:
	FMCPAutomationCommands();

	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	// PIE control
	TSharedPtr<FJsonObject> HandleStartPIE(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleStopPIE(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleGetPIEState(const TSharedPtr<FJsonObject>& Params);

	// PIE world-state query — possessed pawn(s), player controllers + camera POV,
	// filtered actor enumeration in the LIVE PIE world (the editor-world
	// get_actors_in_level / find_actors_by_name can't see PIE-spawned actors).
	TSharedPtr<FJsonObject> HandlePieQuery(const TSharedPtr<FJsonObject>& Params);

	// Screenshot
	TSharedPtr<FJsonObject> HandleTakeScreenshot(const TSharedPtr<FJsonObject>& Params);

	// Frame the active level-editor perspective viewport on an actor (3/4 view).
	TSharedPtr<FJsonObject> HandleFocusActor(const TSharedPtr<FJsonObject>& Params);

	// Read the active level-editor PERSPECTIVE viewport camera pose
	// (location / rotation / FOV / aspect). The "record my framing" primitive —
	// a human frames a shot in the viewport, this captures it.
	TSharedPtr<FJsonObject> HandleGetViewportCamera(const TSharedPtr<FJsonObject>& Params);

	// Reproduce a camera pose inside the LIVE PIE world via a transient CameraActor
	// + view-target swap, then screenshot the game viewport. The deterministic,
	// no-navigation "capture that exact view in-game" primitive.
	TSharedPtr<FJsonObject> HandleCaptureFromPose(const TSharedPtr<FJsonObject>& Params);

	// Input simulation
	TSharedPtr<FJsonObject> HandleSendKeyInput(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleSendMouseInput(const TSharedPtr<FJsonObject>& Params);
	// Enhanced Input action injection (jump/traversal/dash — event-driven actions that
	// synthetic keystrokes can't trigger).
	TSharedPtr<FJsonObject> HandleInjectInputAction(const TSharedPtr<FJsonObject>& Params);

	// Console command
	TSharedPtr<FJsonObject> HandleExecuteConsoleCommand(const TSharedPtr<FJsonObject>& Params);

	// Session tracking — GUID generated per start_pie call
	FString ActiveSessionId;
};

/**
 * Runs a synchronous Live Coding compile on the game thread.
 * Uses Compile(WaitForCompletion) — blocks game thread like Ctrl+Alt+F11.
 * Resolves Promise with a JSON response when complete.
 * Called directly by UMCPBridge::ExecuteCommand.
 */
void AutomationCommands_LiveCodingStart(TSharedPtr<TPromise<FString>> Promise, FMCPLogCollector* LogCollector = nullptr);
