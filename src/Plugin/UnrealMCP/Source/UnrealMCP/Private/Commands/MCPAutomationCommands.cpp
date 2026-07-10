#include "Commands/MCPAutomationCommands.h"
#include "Commands/MCPCommonUtils.h"
#include "MCPLogCollector.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "UnrealClient.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "HighResScreenshot.h"
#include "ImageUtils.h"
#include "FileHelpers.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Engine/GameViewportClient.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "InputKeyEventArgs.h"
#include "Misc/StringOutputDevice.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerInput.h"
#include "Engine/LocalPlayer.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "EnhancedInputSubsystems.h"
#include "Kismet/GameplayStatics.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/OutputDeviceHelper.h"
#include "ILiveCodingModule.h"
#include "Misc/CoreDelegates.h"
#include "Containers/Ticker.h"
#include "LevelEditor.h"       // pie_start in_viewport: resolve the level viewport
#include "IAssetViewport.h"    // FRequestPlaySessionParams::DestinationSlateViewport
#include "Modules/ModuleManager.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

FMCPAutomationCommands::FMCPAutomationCommands()
{
}

TSharedPtr<FJsonObject> FMCPAutomationCommands::HandleCommand(
	const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("pie_start"))
	{
		return HandleStartPIE(Params);
	}
	else if (CommandType == TEXT("pie_stop"))
	{
		return HandleStopPIE(Params);
	}
	else if (CommandType == TEXT("pie_get_state"))
	{
		return HandleGetPIEState(Params);
	}
	else if (CommandType == TEXT("pie_query"))
	{
		return HandlePieQuery(Params);
	}
	else if (CommandType == TEXT("editor_screenshot"))
	{
		return HandleTakeScreenshot(Params);
	}
	else if (CommandType == TEXT("pie_send_keystrokes"))
	{
		return HandleSendKeyInput(Params);
	}
	else if (CommandType == TEXT("pie_send_mouse"))
	{
		return HandleSendMouseInput(Params);
	}
	else if (CommandType == TEXT("pie_inject_input_action"))
	{
		return HandleInjectInputAction(Params);
	}
	else if (CommandType == TEXT("editor_console_exec"))
	{
		return HandleExecuteConsoleCommand(Params);
	}
	else if (CommandType == TEXT("editor_focus_actor"))
	{
		return HandleFocusActor(Params);
	}
	else if (CommandType == TEXT("editor_viewport_get_camera"))
	{
		return HandleGetViewportCamera(Params);
	}
	else if (CommandType == TEXT("pie_capture_from_pose"))
	{
		return HandleCaptureFromPose(Params);
	}

	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown automation command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be one of: start_pie, stop_pie, get_pie_state, pie_query, take_screenshot, send_key_input, send_mouse_input, execute_console_command."));
}

// ---------------------------------------------------------------------------
// Viewport framing — point the level-editor perspective viewport at an actor so
// editor_screenshot (mode=editor) captures it from a good 3/4 angle. This is the
// controllable-camera primitive for asset/scene presentation debugging.
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> FMCPAutomationCommands::HandleFocusActor(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("editor_focus_actor requires 'actor_name'."),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass the actor's name or label, e.g. {\"actor_name\":\"BP_Bike0\"}."));
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No editor world available."),
			EMCPErrorCode::EditorNotReady,
			TEXT("Open a level in the editor first."));
	}

	AActor* Target = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (!A) { continue; }
		if (A->GetName() == ActorName || A->GetActorLabel() == ActorName)
		{
			Target = A;
			break;
		}
	}
	if (!Target)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Actor not found in editor world: %s"), *ActorName),
			EMCPErrorCode::ActorNotFound,
			TEXT("Spawn or place the actor first; match by name or label."));
	}

	// Colliding-only bounds first — excludes far-offset non-colliding components
	// (e.g. a chase-camera / spring-arm) that would otherwise skew the framing of
	// a pawn. Fall back to full bounds if the actor has no colliding components.
	FVector Origin, BoxExtent;
	Target->GetActorBounds(/*bOnlyCollidingComponents*/ true, Origin, BoxExtent);
	if (BoxExtent.IsNearlyZero())
	{
		Target->GetActorBounds(/*bOnlyCollidingComponents*/ false, Origin, BoxExtent);
	}

	// Optional explicit overrides for full control.
	if (Params->HasField(TEXT("focus_location")))
	{
		Origin = FMCPCommonUtils::GetVectorFromJson(Params, TEXT("focus_location"));
	}
	double Radius = FMath::Max(BoxExtent.Size(), 50.0);
	double RadiusOverride = 0.0;
	if (Params->TryGetNumberField(TEXT("radius"), RadiusOverride) && RadiusOverride > 0.0)
	{
		Radius = RadiusOverride;
	}

	double DistanceFactor = 2.5; Params->TryGetNumberField(TEXT("distance_factor"), DistanceFactor);
	double Yaw = 35.0;          Params->TryGetNumberField(TEXT("yaw"), Yaw);
	double Pitch = -20.0;       Params->TryGetNumberField(TEXT("pitch"), Pitch);

	// Camera looks along ViewRotation toward Origin, placed back along that ray.
	const FRotator ViewRotation(Pitch, Yaw, 0.0);
	const FVector LookDir = ViewRotation.Vector();
	const FVector CamLocation = Origin - LookDir * (Radius * DistanceFactor);

	FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
	if (!VC)
	{
		const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
		if (Clients.Num() > 0) { VC = Clients[0]; }
	}
	if (!VC)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No active level-editor viewport."),
			EMCPErrorCode::EditorNotReady,
			TEXT("Focus a perspective viewport in the level editor."));
	}

	VC->SetViewLocation(CamLocation);
	VC->SetViewRotation(ViewRotation);
	if (VC->Viewport)
	{
		VC->Viewport->InvalidateDisplay();
	}
	VC->Invalidate();

	auto VecObj = [](const FVector& V) {
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X); O->SetNumberField(TEXT("y"), V.Y); O->SetNumberField(TEXT("z"), V.Z);
		return O;
	};
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Target->GetActorLabel());
	Result->SetObjectField(TEXT("focus_origin"), VecObj(Origin));
	Result->SetObjectField(TEXT("bounds_extent"), VecObj(BoxExtent));
	Result->SetObjectField(TEXT("camera_location"), VecObj(CamLocation));
	TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
	Rot->SetNumberField(TEXT("pitch"), ViewRotation.Pitch);
	Rot->SetNumberField(TEXT("yaw"), ViewRotation.Yaw);
	Rot->SetNumberField(TEXT("roll"), ViewRotation.Roll);
	Result->SetObjectField(TEXT("camera_rotation"), Rot);
	Result->SetStringField(TEXT("message"),
		TEXT("Level viewport framed on actor. Capture with editor_screenshot mode=\"editor\"."));
	return Result;
}

// ---------------------------------------------------------------------------
// Read the active level-editor perspective viewport camera pose. The "record
// exactly where my editor camera is" primitive: a human frames a shot in the
// viewport, this captures location/rotation/FOV/aspect so an automated pass can
// reproduce the same view in-game (pie_capture_from_pose).
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> FMCPAutomationCommands::HandleGetViewportCamera(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("GEditor not available."),
			EMCPErrorCode::EditorNotReady,
			TEXT("The editor is not ready. Retry after it has finished initializing."));
	}

	// Prefer the last-focused level viewport (the one the user just framed). Fall
	// back to the first perspective viewport, then any viewport at all.
	FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
	if (!VC || !VC->IsPerspective())
	{
		for (FLevelEditorViewportClient* Candidate : GEditor->GetLevelViewportClients())
		{
			if (Candidate && Candidate->IsPerspective()) { VC = Candidate; break; }
		}
	}
	if (!VC)
	{
		const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
		if (Clients.Num() > 0) { VC = Clients[0]; }
	}
	if (!VC)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No active level-editor viewport."),
			EMCPErrorCode::EditorNotReady,
			TEXT("Open and click a perspective viewport in the level editor, then retry."));
	}

	const FVector Loc = VC->GetViewLocation();
	const FRotator Rot = VC->GetViewRotation();
	const float FOV = VC->ViewFOV;
	const bool bOrtho = VC->IsOrtho();

	float Aspect = 0.0f;
	int32 SizeX = 0, SizeY = 0;
	if (VC->Viewport)
	{
		const FIntPoint Sz = VC->Viewport->GetSizeXY();
		SizeX = Sz.X; SizeY = Sz.Y;
		if (Sz.Y > 0) { Aspect = static_cast<float>(Sz.X) / static_cast<float>(Sz.Y); }
	}

	auto VecObj = [](const FVector& V) {
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X); O->SetNumberField(TEXT("y"), V.Y); O->SetNumberField(TEXT("z"), V.Z);
		return O;
	};

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("location"), VecObj(Loc));
	TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
	RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
	RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw);
	RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
	Result->SetObjectField(TEXT("rotation"), RotObj);
	Result->SetNumberField(TEXT("fov"), FOV);
	Result->SetNumberField(TEXT("aspect"), Aspect);
	Result->SetBoolField(TEXT("ortho"), bOrtho);
	TSharedPtr<FJsonObject> SizeObj = MakeShared<FJsonObject>();
	SizeObj->SetNumberField(TEXT("x"), SizeX);
	SizeObj->SetNumberField(TEXT("y"), SizeY);
	Result->SetObjectField(TEXT("viewport_size"), SizeObj);
	Result->SetStringField(TEXT("message"),
		bOrtho
			? TEXT("Read from an ORTHOGRAPHIC viewport — fov is not meaningful. Switch to a perspective viewport for a faithful in-game reproduction.")
			: TEXT("Editor perspective viewport pose captured. Reproduce in-game with pie_capture_from_pose (pass the same location / rotation / fov / aspect)."));
	return Result;
}

// ---------------------------------------------------------------------------
// PIE Control
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMCPAutomationCommands::HandleStartPIE(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("GEditor not available"),
			EMCPErrorCode::Internal,
			TEXT("GEditor is null — the editor is shutting down or has not finished initializing. Automation tools only function within an interactive editor session. If reproducible at editor start, retry after the editor is fully loaded."));
	}

	if (GEditor->IsPlaySessionInProgress())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("PIE session already in progress"),
			EMCPErrorCode::EngineBusy,
			TEXT("A Play-In-Editor session is already running. Call `stop_pie` to end it before requesting a new one, or call `get_pie_state` to check the active session's id. Concurrent PIE sessions are not supported."));
	}

	// Optional: load a specific map before starting PIE. Skipped when the
	// requested map IS the currently-open editor level — re-loading it is
	// pure churn and needlessly destroys/rebuilds the world.
	FString MapToLoad;
	if (Params.IsValid())
	{
		FString MapPath;
		if (Params->TryGetStringField(TEXT("map_path"), MapPath))
		{
			if (!FPackageName::DoesPackageExist(MapPath))
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("Map not found: %s"), *MapPath),
					EMCPErrorCode::AssetNotFound,
					TEXT("`map_path` did not resolve via FPackageName::DoesPackageExist. Verify the path with `list_assets` (asset_type='World'). Paths are case-sensitive and must include `/Game/` prefix. Omit `map_path` to start PIE in the currently-open level."));
			}
			FString PackagePart = MapPath;
			FString ObjectPart;
			MapPath.Split(TEXT("."), &PackagePart, &ObjectPart);
			const UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
			const FString CurrentPackage =
				EditorWorld ? EditorWorld->GetOutermost()->GetName() : FString();
			if (CurrentPackage != PackagePart)
			{
				MapToLoad = MapPath;
			}
		}
	}

	// Optional: run PIE inside the level-editor viewport instead of the floating
	// window from the Play settings. This is what a streamed session wants — the
	// single "Editor" Pixel Streaming producer captures the level viewport, so
	// in-viewport PIE is the only mode a remote phone viewer can actually see
	// (mirrors MCPApplyPieToggleGameThread in MCPStreamingCommands.cpp).
	bool bInViewport = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("in_viewport"), bInViewport);
	}

	// Generate session ID before requesting play
	ActiveSessionId = FGuid::NewGuid().ToString();

	// DEFER the load + play request to the core ticker. This handler executes
	// from a task-graph task on the game thread, which the tick sequencer can
	// pump MID-WORLD-TICK (ReleaseTickGroup → ProcessTasksUntilIdle); calling
	// FEditorFileUtils::LoadMap there destroys the live world while its levels
	// still hold registered tick tasks → the FreeTickTaskLevel assertion
	// (crash of 2026-06-12, full stack in docs/bugs/mcp.md). The core ticker
	// fires at the top of FEngineLoop::Tick — outside any UWorld::Tick — so
	// the world teardown is safe there.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[MapToLoad, bInViewport](float) -> bool
		{
			if (GEditor && !GEditor->IsPlaySessionInProgress())
			{
				if (!MapToLoad.IsEmpty())
				{
					FEditorFileUtils::LoadMap(MapToLoad, false, true);
				}
				FRequestPlaySessionParams SessionParams;
				if (bInViewport)
				{
					// Replicate the toolbar "Play in viewport": target the first
					// active perspective level viewport. Falls back to default
					// (Play-settings window) when none resolves.
					if (FLevelEditorModule* LevelEditorModule =
						FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
					{
						const TSharedPtr<IAssetViewport> ActiveLevelViewport =
							LevelEditorModule->GetFirstActiveViewport();
						if (ActiveLevelViewport.IsValid() &&
							ActiveLevelViewport->GetAssetViewportClient().IsPerspective())
						{
							SessionParams.DestinationSlateViewport = ActiveLevelViewport;
						}
					}
				}
				GEditor->RequestPlaySession(SessionParams);
			}
			return false;   // one-shot
		}), 0.0f);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("starting"));
	Result->SetStringField(TEXT("session_id"), ActiveSessionId);
	Result->SetStringField(TEXT("message"),
		TEXT("PIE session requested. Poll get_pie_state to check when ready."));
	return Result;
}

TSharedPtr<FJsonObject> FMCPAutomationCommands::HandleStopPIE(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("GEditor not available"),
			EMCPErrorCode::Internal,
			TEXT("GEditor is null — the editor is shutting down or has not finished initializing. Automation tools only function within an interactive editor session. If reproducible at editor start, retry after the editor is fully loaded."));
	}

	if (!GEditor->IsPlaySessionInProgress())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No PIE session in progress"),
			EMCPErrorCode::NotInPie,
			TEXT("No Play-In-Editor session is running, so there is nothing to stop. Call `get_pie_state` to verify, or `start_pie` to start a session."));
	}

	// If a session_id is provided, validate it matches the active session
	if (Params.IsValid())
	{
		FString RequestedId;
		if (Params->TryGetStringField(TEXT("session_id"), RequestedId))
		{
			if (RequestedId != ActiveSessionId)
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("Session ID mismatch: requested %s, active %s"), *RequestedId, *ActiveSessionId),
					EMCPErrorCode::InvalidArgument,
					TEXT("The `session_id` you passed doesn't match the active PIE session. Call `get_pie_state` to get the current `session_id`, or omit `session_id` to stop whichever session is active."));
			}
		}
	}

	FString StoppedSessionId = ActiveSessionId;
	ActiveSessionId.Empty();
	GEditor->RequestEndPlayMap();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("stopping"));
	Result->SetStringField(TEXT("session_id"), StoppedSessionId);
	Result->SetStringField(TEXT("message"),
		TEXT("PIE stop requested. Session will end within a few frames."));
	return Result;
}

TSharedPtr<FJsonObject> FMCPAutomationCommands::HandleGetPIEState(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("GEditor not available"),
			EMCPErrorCode::Internal,
			TEXT("GEditor is null — the editor is shutting down or has not finished initializing. Automation tools only function within an interactive editor session. If reproducible at editor start, retry after the editor is fully loaded."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	bool bIsRunning = GEditor->IsPlaySessionInProgress();
	Result->SetBoolField(TEXT("is_running"), bIsRunning);

	if (bIsRunning)
	{
		Result->SetStringField(TEXT("session_id"), ActiveSessionId);

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World())
			{
				Result->SetStringField(TEXT("world_name"), Context.World()->GetName());
				Result->SetStringField(TEXT("map_name"), Context.World()->GetMapName());
				break;
			}
		}
	}
	else
	{
		// PIE ended externally (user clicked stop) — clear stale session
		ActiveSessionId.Empty();
	}

	return Result;
}

// ---------------------------------------------------------------------------
// PIE world-state query
// ---------------------------------------------------------------------------
//
// query: "summary" (default) | "players"/"player"/"pawn" | "actors" | "all".
//   summary -> world/map, total_actors, num_player_controllers, possessed_pawns[]
//   players -> per player controller: pawn (name/label/class/loc/rot) + velocity,
//              view_target, camera_pov (GetPlayerViewPoint). This is how you get the
//              "currently piloted pawn" without guessing class names.
//   actors  -> filtered (substring on name/label/class), capped by `limit`.
//   all     -> everything.
// Reads the LIVE PIE world — fills the gap left by editor-world get_actors_in_level
// / find_actors_by_name (which never see PIE-spawned actors).

TSharedPtr<FJsonObject> FMCPAutomationCommands::HandlePieQuery(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor || !GEngine)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("GEditor/GEngine not available"),
			EMCPErrorCode::Internal,
			TEXT("The editor is shutting down or has not finished initializing. Retry once loaded."));
	}

	UWorld* World = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			World = Context.World();
			break;
		}
	}
	if (!World)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("PIE is not running"),
			EMCPErrorCode::NotInPie,
			TEXT("pie_query reads the live Play-In-Editor world (possessed pawn, player controllers, actors). Start PIE (start_pie or the editor Play button) and retry."));
	}

	FString Query = TEXT("summary");
	FString Filter;
	int32 Limit = 200;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("query"), Query);
		Params->TryGetStringField(TEXT("filter"), Filter);
		double L = 0;
		if (Params->TryGetNumberField(TEXT("limit"), L)) Limit = FMath::Clamp((int32)L, 1, 5000);
	}
	const bool bAll     = Query.Equals(TEXT("all"), ESearchCase::IgnoreCase);
	const bool bSummary = bAll || Query.Equals(TEXT("summary"), ESearchCase::IgnoreCase);
	const bool bPlayers = bAll
		|| Query.Equals(TEXT("players"), ESearchCase::IgnoreCase)
		|| Query.Equals(TEXT("player"), ESearchCase::IgnoreCase)
		|| Query.Equals(TEXT("pawn"), ESearchCase::IgnoreCase);
	const bool bActors  = bAll || Query.Equals(TEXT("actors"), ESearchCase::IgnoreCase);

	auto Vec = [](const FVector& V)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X); O->SetNumberField(TEXT("y"), V.Y); O->SetNumberField(TEXT("z"), V.Z);
		return O;
	};
	auto Rot = [](const FRotator& R)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("pitch"), R.Pitch); O->SetNumberField(TEXT("yaw"), R.Yaw); O->SetNumberField(TEXT("roll"), R.Roll);
		return O;
	};
	auto ActorBrief = [&](AActor* A, bool bWithRot)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), A->GetName());
		O->SetStringField(TEXT("label"), A->GetActorLabel());
		O->SetStringField(TEXT("class"), A->GetClass()->GetName());
		O->SetObjectField(TEXT("location"), Vec(A->GetActorLocation()));
		if (bWithRot) O->SetObjectField(TEXT("rotation"), Rot(A->GetActorRotation()));
		return O;
	};

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("is_running"), true);
	Result->SetStringField(TEXT("world_name"), World->GetName());
	Result->SetStringField(TEXT("map_name"), World->GetMapName());

	// Player controllers + possessed pawns.
	int32 NumPCs = 0;
	TArray<TSharedPtr<FJsonValue>> Players;
	TArray<TSharedPtr<FJsonValue>> PossessedBrief;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC) continue;
		++NumPCs;
		APawn* Pawn = PC->GetPawn();
		if (Pawn)
		{
			PossessedBrief.Add(MakeShared<FJsonValueObject>(ActorBrief(Pawn, false)));
		}
		if (bPlayers)
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("controller"), PC->GetName());
			if (Pawn)
			{
				P->SetObjectField(TEXT("pawn"), ActorBrief(Pawn, true));
				P->SetObjectField(TEXT("pawn_velocity"), Vec(Pawn->GetVelocity()));
			}
			if (AActor* VT = PC->GetViewTarget())
			{
				P->SetStringField(TEXT("view_target"), VT->GetName());
			}
			FVector POVLoc = FVector::ZeroVector;
			FRotator POVRot = FRotator::ZeroRotator;
			PC->GetPlayerViewPoint(POVLoc, POVRot);
			TSharedPtr<FJsonObject> POV = MakeShared<FJsonObject>();
			POV->SetObjectField(TEXT("location"), Vec(POVLoc));
			POV->SetObjectField(TEXT("rotation"), Rot(POVRot));
			P->SetObjectField(TEXT("camera_pov"), POV);
			Players.Add(MakeShared<FJsonValueObject>(P));
		}
	}
	if (bPlayers)
	{
		Result->SetArrayField(TEXT("players"), Players);
	}

	// Actor enumeration + total count.
	if (bActors || bSummary)
	{
		int32 Total = 0;
		int32 Emitted = 0;
		TArray<TSharedPtr<FJsonValue>> Actors;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			++Total;
			if (bActors && Emitted < Limit)
			{
				AActor* A = *It;
				const bool bMatch = Filter.IsEmpty()
					|| A->GetName().Contains(Filter)
					|| A->GetClass()->GetName().Contains(Filter)
					|| A->GetActorLabel().Contains(Filter);
				if (bMatch)
				{
					Actors.Add(MakeShared<FJsonValueObject>(ActorBrief(A, false)));
					++Emitted;
				}
			}
		}
		if (bActors)
		{
			Result->SetArrayField(TEXT("actors"), Actors);
			Result->SetNumberField(TEXT("actors_returned"), Emitted);
			Result->SetBoolField(TEXT("actors_truncated"), Emitted >= Limit);
			if (!Filter.IsEmpty()) Result->SetStringField(TEXT("filter"), Filter);
		}
		if (bSummary)
		{
			Result->SetNumberField(TEXT("total_actors"), Total);
		}
	}

	if (bSummary)
	{
		Result->SetNumberField(TEXT("num_player_controllers"), NumPCs);
		Result->SetArrayField(TEXT("possessed_pawns"), PossessedBrief);
	}

	return Result;
}

// ---------------------------------------------------------------------------
// Screenshot
// ---------------------------------------------------------------------------

// File-scope helper: capture the LIVE PIE game viewport (3D scene + composited
// Slate UI) to a PNG via the Windows PrintWindow API, cropped to the game viewport
// widget rect (excludes editor chrome). Mirrors the synchronous PIE branch of
// HandleTakeScreenshot — used by pie_capture_from_pose's deferred (post-view-swap)
// capture. Must run on the game thread. Returns true on a written file.
static bool MCPCaptureGameViewportToFile(const FString& FullPath)
{
	if (!GEngine || !GEngine->GameViewport) { return false; }
	TSharedPtr<SWindow> Window = GEngine->GameViewport->GetWindow();
	if (!Window.IsValid() || !Window->GetNativeWindow().IsValid()) { return false; }

	HWND Hwnd = static_cast<HWND>(Window->GetNativeWindow()->GetOSWindowHandle());
	RECT ClientRect;
	GetClientRect(Hwnd, &ClientRect);
	const int32 WinWidth = ClientRect.right - ClientRect.left;
	const int32 WinHeight = ClientRect.bottom - ClientRect.top;
	if (WinWidth <= 0 || WinHeight <= 0) { return false; }

	HDC WindowDC = GetDC(Hwnd);
	HDC MemDC = CreateCompatibleDC(WindowDC);
	HBITMAP HBitmap = CreateCompatibleBitmap(WindowDC, WinWidth, WinHeight);
	HBITMAP OldBitmap = static_cast<HBITMAP>(SelectObject(MemDC, HBitmap));

	::PrintWindow(Hwnd, MemDC, 3); // PW_CLIENTONLY | PW_RENDERFULLCONTENT

	BITMAPINFOHEADER BmpHeader = {};
	BmpHeader.biSize = sizeof(BITMAPINFOHEADER);
	BmpHeader.biWidth = WinWidth;
	BmpHeader.biHeight = -WinHeight; // top-down
	BmpHeader.biPlanes = 1;
	BmpHeader.biBitCount = 32;
	BmpHeader.biCompression = BI_RGB;

	TArray<FColor> FullPixels;
	FullPixels.SetNum(WinWidth * WinHeight);
	GetDIBits(MemDC, HBitmap, 0, WinHeight, FullPixels.GetData(),
		reinterpret_cast<BITMAPINFO*>(&BmpHeader), DIB_RGB_COLORS);

	// FColor is BGRA in memory and PNGCompressImageArray consumes BGRA — do NOT swap
	// R/B (double-swap bug, see docs/bugs/mcp.md); just force alpha opaque.
	for (FColor& Px : FullPixels) { Px.A = 255; }

	SelectObject(MemDC, OldBitmap);
	DeleteObject(HBitmap);
	DeleteDC(MemDC);
	ReleaseDC(Hwnd, WindowDC);

	// Crop to the game viewport widget (excludes editor chrome).
	int32 SaveWidth = WinWidth;
	int32 SaveHeight = WinHeight;
	TArray<FColor>* SavePixels = &FullPixels;
	TArray<FColor> CroppedPixels;
	TSharedPtr<SViewport> ViewportWidget = GEngine->GameViewport->GetGameViewportWidget();
	if (ViewportWidget.IsValid())
	{
		FGeometry Geom = ViewportWidget->GetCachedGeometry();
		FVector2D AbsPos = Geom.GetAbsolutePosition();
		FVector2D AbsSize = Geom.GetAbsoluteSize();

		POINT WindowOrigin = {0, 0};
		ClientToScreen(Hwnd, &WindowOrigin);
		int32 CropX = FMath::RoundToInt32(AbsPos.X) - WindowOrigin.x;
		int32 CropY = FMath::RoundToInt32(AbsPos.Y) - WindowOrigin.y;
		int32 CropW = FMath::RoundToInt32(AbsSize.X);
		int32 CropH = FMath::RoundToInt32(AbsSize.Y);

		CropX = FMath::Clamp(CropX, 0, WinWidth - 1);
		CropY = FMath::Clamp(CropY, 0, WinHeight - 1);
		CropW = FMath::Min(CropW, WinWidth - CropX);
		CropH = FMath::Min(CropH, WinHeight - CropY);

		if (CropW > 0 && CropH > 0)
		{
			CroppedPixels.SetNum(CropW * CropH);
			for (int32 Row = 0; Row < CropH; ++Row)
			{
				FMemory::Memcpy(
					&CroppedPixels[Row * CropW],
					&FullPixels[(CropY + Row) * WinWidth + CropX],
					CropW * sizeof(FColor));
			}
			SavePixels = &CroppedPixels;
			SaveWidth = CropW;
			SaveHeight = CropH;
		}
	}

	TArray64<uint8> PngData;
	FImageUtils::PNGCompressImageArray(SaveWidth, SaveHeight, *SavePixels, PngData);
	return FFileHelper::SaveArrayToFile(
		MakeArrayView(PngData.GetData(), PngData.Num()), *FullPath);
}

TSharedPtr<FJsonObject> FMCPAutomationCommands::HandleTakeScreenshot(
	const TSharedPtr<FJsonObject>& Params)
{
	FString Directory;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("directory"), Directory))
	{
		Directory = FPaths::ScreenShotDir();
	}

	FString Filename;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("filename"), Filename))
	{
		Filename = FString::Printf(TEXT("MCP_Screenshot_%s"),
			*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}

	if (!Filename.EndsWith(TEXT(".png")))
	{
		Filename += TEXT(".png");
	}

	FString FullPath = FPaths::Combine(Directory, Filename);
	FullPath = FPaths::ConvertRelativePathToFull(FullPath);

	// Ensure directory exists
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*FPaths::GetPath(FullPath));

	// mode: "editor" = full editor window (all panels, menus, toolbar),
	//        "viewport" (default) = game viewport only
	FString Mode = TEXT("viewport");
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("mode"), Mode);
	}
	const bool bEditorMode = Mode.Equals(TEXT("editor"), ESearchCase::IgnoreCase);

	bool bCapturedSync = false;

	// --- Helper lambda: capture an HWND via Windows PrintWindow API ---
	auto CaptureWindowToFile = [&FullPath](HWND Hwnd) -> bool
	{
		RECT ClientRect;
		GetClientRect(Hwnd, &ClientRect);
		int32 WinWidth = ClientRect.right - ClientRect.left;
		int32 WinHeight = ClientRect.bottom - ClientRect.top;
		if (WinWidth <= 0 || WinHeight <= 0)
		{
			return false;
		}

		HDC WindowDC = GetDC(Hwnd);
		HDC MemDC = CreateCompatibleDC(WindowDC);
		HBITMAP HBitmap = CreateCompatibleBitmap(WindowDC, WinWidth, WinHeight);
		HBITMAP OldBitmap = static_cast<HBITMAP>(SelectObject(MemDC, HBitmap));

		// PW_CLIENTONLY (1) = skip title bar; PW_RENDERFULLCONTENT (2) = include DX surfaces
		::PrintWindow(Hwnd, MemDC, 3);

		BITMAPINFOHEADER BmpHeader = {};
		BmpHeader.biSize = sizeof(BITMAPINFOHEADER);
		BmpHeader.biWidth = WinWidth;
		BmpHeader.biHeight = -WinHeight; // top-down
		BmpHeader.biPlanes = 1;
		BmpHeader.biBitCount = 32;
		BmpHeader.biCompression = BI_RGB;

		TArray<FColor> Pixels;
		Pixels.SetNum(WinWidth * WinHeight);
		GetDIBits(MemDC, HBitmap, 0, WinHeight, Pixels.GetData(),
			reinterpret_cast<BITMAPINFO*>(&BmpHeader), DIB_RGB_COLORS);

		// GetDIBits returns BGRA with alpha 0. FColor is already BGRA in memory and
		// PNGCompressImageArray consumes it as ERGBFormat::BGRA, so the channels are
		// correct as-is — only force alpha opaque. (Do NOT swap R/B: that double-swaps
		// and writes a channel-swapped PNG. See docs/bugs/mcp.md.)
		for (FColor& Px : Pixels)
		{
			Px.A = 255;
		}

		SelectObject(MemDC, OldBitmap);
		DeleteObject(HBitmap);
		DeleteDC(MemDC);
		ReleaseDC(Hwnd, WindowDC);

		TArray64<uint8> PngData;
		FImageUtils::PNGCompressImageArray(WinWidth, WinHeight, Pixels, PngData);
		FFileHelper::SaveArrayToFile(
			MakeArrayView(PngData.GetData(), PngData.Num()), *FullPath);
		return true;
	};

	if (bEditorMode)
	{
		// Capture the full editor window — all panels, menus, toolbar, viewport.
		// Use the active top-level Slate window (the main editor frame).
		TSharedPtr<SWindow> Window = FSlateApplication::Get().GetActiveTopLevelWindow();
		if (Window.IsValid() && Window->GetNativeWindow().IsValid())
		{
			HWND Hwnd = static_cast<HWND>(Window->GetNativeWindow()->GetOSWindowHandle());
			bCapturedSync = CaptureWindowToFile(Hwnd);
		}
	}
	else if (GEditor && GEditor->IsPlaySessionInProgress() && GEngine && GEngine->GameViewport)
	{
		// During PIE, capture the composited window (3D scene + Slate UI) via Windows API,
		// then crop to the game viewport widget rect to exclude editor chrome.
		// FViewport::ReadPixels only gets the 3D scene — UMG widgets are composited by Slate
		// in a later pass, so we must capture from the OS window surface instead.
		TSharedPtr<SWindow> Window = GEngine->GameViewport->GetWindow();
		if (Window.IsValid() && Window->GetNativeWindow().IsValid())
		{
			HWND Hwnd = static_cast<HWND>(Window->GetNativeWindow()->GetOSWindowHandle());
			RECT ClientRect;
			GetClientRect(Hwnd, &ClientRect);
			int32 WinWidth = ClientRect.right - ClientRect.left;
			int32 WinHeight = ClientRect.bottom - ClientRect.top;

			if (WinWidth > 0 && WinHeight > 0)
			{
				HDC WindowDC = GetDC(Hwnd);
				HDC MemDC = CreateCompatibleDC(WindowDC);
				HBITMAP HBitmap = CreateCompatibleBitmap(WindowDC, WinWidth, WinHeight);
				HBITMAP OldBitmap = static_cast<HBITMAP>(SelectObject(MemDC, HBitmap));

				::PrintWindow(Hwnd, MemDC, 3);

				BITMAPINFOHEADER BmpHeader = {};
				BmpHeader.biSize = sizeof(BITMAPINFOHEADER);
				BmpHeader.biWidth = WinWidth;
				BmpHeader.biHeight = -WinHeight; // top-down
				BmpHeader.biPlanes = 1;
				BmpHeader.biBitCount = 32;
				BmpHeader.biCompression = BI_RGB;

				TArray<FColor> FullPixels;
				FullPixels.SetNum(WinWidth * WinHeight);
				GetDIBits(MemDC, HBitmap, 0, WinHeight, FullPixels.GetData(),
					reinterpret_cast<BITMAPINFO*>(&BmpHeader), DIB_RGB_COLORS);

				for (FColor& Px : FullPixels)
				{
					/* no R/B swap: FColor is BGRA, PNGCompressImageArray reads BGRA — see docs/bugs/mcp.md */
					Px.A = 255;
				}

				SelectObject(MemDC, OldBitmap);
				DeleteObject(HBitmap);
				DeleteDC(MemDC);
				ReleaseDC(Hwnd, WindowDC);

				// --- Crop to game viewport widget (excludes editor chrome) ---
				TSharedPtr<SViewport> ViewportWidget = GEngine->GameViewport->GetGameViewportWidget();
				int32 SaveWidth = WinWidth;
				int32 SaveHeight = WinHeight;
				TArray<FColor>* SavePixels = &FullPixels;
				TArray<FColor> CroppedPixels;

				if (ViewportWidget.IsValid())
				{
					FGeometry Geom = ViewportWidget->GetCachedGeometry();
					FVector2D AbsPos = Geom.GetAbsolutePosition();
					FVector2D AbsSize = Geom.GetAbsoluteSize();

					POINT WindowOrigin = {0, 0};
					ClientToScreen(Hwnd, &WindowOrigin);
					int32 CropX = FMath::RoundToInt32(AbsPos.X) - WindowOrigin.x;
					int32 CropY = FMath::RoundToInt32(AbsPos.Y) - WindowOrigin.y;
					int32 CropW = FMath::RoundToInt32(AbsSize.X);
					int32 CropH = FMath::RoundToInt32(AbsSize.Y);

					CropX = FMath::Clamp(CropX, 0, WinWidth - 1);
					CropY = FMath::Clamp(CropY, 0, WinHeight - 1);
					CropW = FMath::Min(CropW, WinWidth - CropX);
					CropH = FMath::Min(CropH, WinHeight - CropY);

					if (CropW > 0 && CropH > 0)
					{
						CroppedPixels.SetNum(CropW * CropH);
						for (int32 Row = 0; Row < CropH; ++Row)
						{
							FMemory::Memcpy(
								&CroppedPixels[Row * CropW],
								&FullPixels[(CropY + Row) * WinWidth + CropX],
								CropW * sizeof(FColor));
						}
						SavePixels = &CroppedPixels;
						SaveWidth = CropW;
						SaveHeight = CropH;
					}
				}

				TArray64<uint8> PngData;
				FImageUtils::PNGCompressImageArray(SaveWidth, SaveHeight, *SavePixels, PngData);
				FFileHelper::SaveArrayToFile(
					MakeArrayView(PngData.GetData(), PngData.Num()), *FullPath);
				bCapturedSync = true;
			}
		}
	}

	if (!bCapturedSync)
	{
		// Editor viewport fallback (async): FScreenshotRequest is serviced at
		// end-of-frame inside FViewport::Draw. A non-realtime editor level viewport
		// only redraws when invalidated, so if nothing forces a redraw the request is
		// never serviced and NO file is ever written (GAP-007 — the silent no-op).
		// Force a short burst of realtime frames + invalidate the active level
		// viewport(s) so a frame is actually scheduled to render (and thus to service
		// the request). RequestRealTimeFrames is non-persistent: it auto-clears once
		// GFrameCounter advances past the requested count, so the editor's realtime
		// setting is left untouched. (Still partial by nature — a fully occluded /
		// minimized window may not composite a frame at all; the bridge then returns
		// a real `timeout` error instead of a false "requested".)
		if (GCurrentLevelEditingViewportClient)
		{
			GCurrentLevelEditingViewportClient->RequestRealTimeFrames(120);
			GCurrentLevelEditingViewportClient->Invalidate();
		}
		else if (GEditor)
		{
			for (FLevelEditorViewportClient* VPC : GEditor->GetLevelViewportClients())
			{
				if (VPC)
				{
					VPC->RequestRealTimeFrames(120);
					VPC->Invalidate();
				}
			}
		}

		FScreenshotRequest::RequestScreenshot(FullPath, false, false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("file_path"), FullPath);
	// GAP-043: also emit `path` (the resolved absolute output path) — callers that key
	// off result.path previously saw null because only file_path was set. Both carry it.
	Result->SetStringField(TEXT("path"), FullPath);
	Result->SetStringField(TEXT("status"), bCapturedSync ? TEXT("captured") : TEXT("requested"));
	Result->SetStringField(TEXT("message"),
		bCapturedSync
			? (bEditorMode
				? TEXT("Screenshot captured synchronously — full editor window.")
				: TEXT("Screenshot captured synchronously from PIE game viewport."))
			: TEXT("Screenshot requested from editor viewport (forced a render). The bridge ")
			  TEXT("confirms the output file on the server thread before returning."));
	return Result;
}

// ---------------------------------------------------------------------------
// Reproduce a camera pose inside the LIVE PIE world and screenshot it. Spawns a
// transient CameraActor at the pose, swaps the player's view target to it (blend
// 0), waits a few frames for the new view to composite, then captures the game
// viewport — and (by default) restores the original view target and destroys the
// temp camera. This is the deterministic, no-navigation capture rig: pair it with
// editor_viewport_get_camera to turn a human-framed editor shot into a reproducible
// in-game screenshot. Async by nature (defers the grab to the core ticker) — returns
// status "requested"; the bridge confirms the output file before returning.
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> FMCPAutomationCommands::HandleCaptureFromPose(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor || !GEditor->IsPlaySessionInProgress())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No PIE session in progress."),
			EMCPErrorCode::NotInPie,
			TEXT("pie_capture_from_pose reproduces a pose INSIDE the running game. Start PIE first (pie_start), then retry."));
	}
	if (!Params.IsValid() || !Params->HasField(TEXT("location")) || !Params->HasField(TEXT("rotation")))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("pie_capture_from_pose requires 'location' and 'rotation'."),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass location {x,y,z} and rotation {pitch,yaw,roll} (typically straight from editor_viewport_get_camera)."));
	}

	UWorld* PieWorld = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			PieWorld = Context.World();
			break;
		}
	}
	if (!PieWorld)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("PIE is starting but no PIE world is available yet."),
			EMCPErrorCode::EngineBusy,
			TEXT("The PIE world has not finished spawning. Poll pie_get_state until is_running, then retry."));
	}
	APlayerController* PC = PieWorld->GetFirstPlayerController();
	if (!PC)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No player controller in the PIE world."),
			EMCPErrorCode::NotInPie,
			TEXT("The PIE world has no player controller to drive the view. Ensure a PlayerController/Pawn exists."));
	}

	const FVector Loc = FMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
	const FRotator Rot = FMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
	double Fov = 0.0; const bool bHasFov = Params->TryGetNumberField(TEXT("fov"), Fov);
	double Aspect = 0.0; const bool bHasAspect = Params->TryGetNumberField(TEXT("aspect"), Aspect);
	bool bRestore = true; Params->TryGetBoolField(TEXT("restore"), bRestore);

	// Resolve output path (mirror HandleTakeScreenshot's defaults).
	FString Directory;
	if (!Params->TryGetStringField(TEXT("directory"), Directory) || Directory.IsEmpty())
	{
		Directory = FPaths::ScreenShotDir();
	}
	FString Filename;
	if (!Params->TryGetStringField(TEXT("filename"), Filename) || Filename.IsEmpty())
	{
		Filename = FString::Printf(TEXT("MCP_PoseCapture_%s"),
			*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}
	if (!Filename.EndsWith(TEXT(".png"))) { Filename += TEXT(".png"); }
	FString FullPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(Directory, Filename));
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(FullPath));

	// Spawn a transient camera at the pose and point the player's view at it.
	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transient;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ACameraActor* Cam = PieWorld->SpawnActor<ACameraActor>(
		ACameraActor::StaticClass(), Loc, Rot, SpawnParams);
	if (!Cam)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to spawn capture camera in the PIE world."),
			EMCPErrorCode::Internal,
			TEXT("SpawnActor<ACameraActor> returned null. Retry; if persistent, the PIE world may be tearing down."));
	}
	if (UCameraComponent* CamComp = Cam->GetCameraComponent())
	{
		if (bHasFov && Fov > 0.0) { CamComp->SetFieldOfView(static_cast<float>(Fov)); }
		if (bHasAspect && Aspect > 0.0)
		{
			CamComp->SetConstraintAspectRatio(true);
			CamComp->SetAspectRatio(static_cast<float>(Aspect));
		}
	}

	AActor* PrevViewTarget = PC->GetViewTarget();
	PC->SetViewTargetWithBlend(Cam, 0.0f);

	// Defer the grab so the swapped view target actually composites a frame before
	// we PrintWindow the OS surface. Restore + clean up after the grab (default).
	// Weak ptrs guard against a PIE teardown between now and the ticker firing.
	TWeakObjectPtr<ACameraActor> WeakCam(Cam);
	TWeakObjectPtr<APlayerController> WeakPC(PC);
	TWeakObjectPtr<AActor> WeakPrev(PrevViewTarget);
	TSharedPtr<int32> FramesLeft = MakeShared<int32>(3);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[FullPath, WeakCam, WeakPC, WeakPrev, bRestore, FramesLeft](float) -> bool
		{
			if (--(*FramesLeft) > 0) { return true; } // let the new view render first
			MCPCaptureGameViewportToFile(FullPath);
			if (bRestore)
			{
				if (WeakPC.IsValid() && WeakPrev.IsValid())
				{
					WeakPC->SetViewTargetWithBlend(WeakPrev.Get(), 0.0f);
				}
				if (WeakCam.IsValid()) { WeakCam->Destroy(); }
			}
			return false; // one-shot
		}), 0.0f);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("file_path"), FullPath);
	Result->SetStringField(TEXT("path"), FullPath);
	Result->SetStringField(TEXT("status"), TEXT("requested"));
	Result->SetBoolField(TEXT("restored"), bRestore);
	Result->SetStringField(TEXT("message"),
		TEXT("In-game capture requested: view target swapped to a transient camera at the pose; ")
		TEXT("the bridge confirms the output file on the server thread before returning."));
	return Result;
}

// ---------------------------------------------------------------------------
// Input Simulation
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMCPAutomationCommands::HandleSendKeyInput(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing params"),
			EMCPErrorCode::InvalidArgument,
			TEXT("This handler requires a params object. Pass a JSON object with at minimum the documented required fields for this tool."));
	}

	FString KeyString;
	if (!Params->TryGetStringField(TEXT("key"), KeyString))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'key' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`key` is required (string, an Unreal FKey name — e.g. `W`, `Space`, `LeftShift`, `LeftMouseButton`). Use the engine's FKey string form. Check EKeys.cpp in the engine source for the full enumeration."));
	}

	FString EventTypeStr;
	if (!Params->TryGetStringField(TEXT("event_type"), EventTypeStr))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'event_type' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`event_type` is required (string). Valid values for send_key_input: `pressed`, `released`. A press-and-release pair requires two calls."));
	}

	if (EventTypeStr != TEXT("pressed") && EventTypeStr != TEXT("released"))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Invalid event_type: %s. Expected: pressed, released"), *EventTypeStr),
			EMCPErrorCode::InvalidArgument,
			TEXT("`event_type` for send_key_input must be exactly `pressed` or `released` (case-sensitive). To simulate a tap, send two calls — one with `pressed`, then one with `released`."));
	}

	if (KeyString.Len() >= NAME_SIZE)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Invalid key: name too long (%d chars)"), KeyString.Len()),
			EMCPErrorCode::InvalidArgument,
			TEXT("`key` exceeds the engine's FName length limit (1024). A valid FKey name is short (e.g. `W`, `Space`, `LeftMouseButton`); constructing an FName from a longer string is a fatal engine assert."));
	}

	FKey Key(*KeyString);
	if (!Key.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Invalid key: %s"), *KeyString),
			EMCPErrorCode::InvalidArgument,
			TEXT("`key` did not resolve to a valid FKey via `FKey(*KeyString).IsValid()`. Use the canonical short-name form (`W`, `Space`, `Escape`, `LeftShift`, `F1`, `LeftMouseButton`, `Gamepad_FaceButton_Bottom`). Case-insensitive. See `EKeys` static FKey table in engine source for the complete set."));
	}

	// Parse modifiers
	bool bShift = false, bCtrl = false, bAlt = false, bCmd = false;
	const TSharedPtr<FJsonObject>* ModifiersObj = nullptr;
	if (Params->TryGetObjectField(TEXT("modifiers"), ModifiersObj) && ModifiersObj)
	{
		(*ModifiersObj)->TryGetBoolField(TEXT("shift"), bShift);
		(*ModifiersObj)->TryGetBoolField(TEXT("ctrl"), bCtrl);
		(*ModifiersObj)->TryGetBoolField(TEXT("alt"), bAlt);
		(*ModifiersObj)->TryGetBoolField(TEXT("cmd"), bCmd);
	}

	bool bPIEActive = GEditor && GEditor->IsPlaySessionInProgress();

	// Opt-in (default false): focus the game viewport BEFORE injecting so a *held* key
	// reaches polled input (APlayerController::IsInputKeyDown). When the editor — not the
	// PIE viewport — holds Slate keyboard focus, UGameViewportClient::LostFocus calls
	// APlayerController::FlushPressedKeys (UInputSettings::bShouldFlushPressedKeysOnViewportFocusLost
	// defaults true), zeroing the key state before a polling pawn reads it. The engine-blessed
	// remedy is FSlateApplication::SetAllUserFocusToGameViewport (SlateApplication.cpp:2625,
	// as used by UGameEngine / WidgetBlueprintLibrary SetInputMode*). This STEALS keyboard focus
	// from the editor window, so it is off by default to preserve current behavior. The call
	// no-ops internally when no game viewport widget is valid.
	bool bFocusViewport = false;
	Params->TryGetBoolField(TEXT("focus_viewport"), bFocusViewport);
	if (bFocusViewport && bPIEActive && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetAllUserFocusToGameViewport();
	}

	// During PIE, inject via PlayerController for gameplay input fidelity
	if (bPIEActive)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World())
			{
				APlayerController* PC = Context.World()->GetFirstPlayerController();
				if (PC)
				{
					EInputEvent InputEvent = (EventTypeStr == TEXT("pressed"))
						? IE_Pressed : IE_Released;
					FInputKeyEventArgs KeyArgs = FInputKeyEventArgs::CreateSimulated(Key, InputEvent, 1.0f);
					PC->InputKey(KeyArgs);
				}
				break;
			}
		}
	}
	else
	{
		// Editor-level: inject through Slate
		FModifierKeysState ModifierKeys(
			bShift, bShift,  // left/right shift
			bCtrl, bCtrl,    // left/right ctrl
			bAlt, bAlt,      // left/right alt
			bCmd, bCmd,       // left/right cmd
			false             // caps lock
		);

		const uint32 CharCode = 0;
		const uint32 KeyCode = 0;
		FKeyEvent KeyEvent(Key, ModifierKeys, 0, false, CharCode, KeyCode);

		if (EventTypeStr == TEXT("pressed"))
		{
			FSlateApplication::Get().ProcessKeyDownEvent(KeyEvent);
		}
		else
		{
			FSlateApplication::Get().ProcessKeyUpEvent(KeyEvent);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("key"), KeyString);
	Result->SetStringField(TEXT("event_type"), EventTypeStr);
	Result->SetBoolField(TEXT("pie_active"), bPIEActive);
	return Result;
}

// Inject an Enhanced Input action into the live PIE player. This is the typed way to
// trigger "Started"/event-driven input actions (jump, traversal, dash) that synthetic
// keystrokes don't reach (only continuous/axis input survives key simulation). Loads the
// UInputAction at action_path and injects RawValue for one evaluation on the PIE player's
// EnhancedInput subsystem (a one-shot press).
TSharedPtr<FJsonObject> FMCPAutomationCommands::HandleInjectInputAction(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing params"), EMCPErrorCode::InvalidArgument,
			TEXT("Pass { action_path: \"/Game/.../IA_Foo\", value?: number }."));
	}

	FString ActionPath;
	if (!Params->TryGetStringField(TEXT("action_path"), ActionPath))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing action_path"), EMCPErrorCode::InvalidArgument,
			TEXT("action_path must be the /Game/... path of a UInputAction asset (e.g. an IA_* asset)."));
	}

	double Value = 1.0;
	Params->TryGetNumberField(TEXT("value"), Value);

	if (!(GEditor && GEditor->IsPlaySessionInProgress()))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("PIE is not running"), EMCPErrorCode::InvalidArgument,
			TEXT("Enhanced Input injection targets the live PIE player. Start PIE and retry."));
	}

	UInputAction* Action = LoadObject<UInputAction>(nullptr, *ActionPath);
	if (!Action)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Could not load InputAction"), EMCPErrorCode::AssetNotFound,
			TEXT("action_path did not resolve to a UInputAction. Verify the path with asset_list (class_filter=InputAction)."));
	}

	bool bInjected = false;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			if (APlayerController* PC = Context.World()->GetFirstPlayerController())
			{
				if (ULocalPlayer* LP = PC->GetLocalPlayer())
				{
					if (UEnhancedInputLocalPlayerSubsystem* Sub = LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
					{
						Sub->InjectInputForAction(Action, FInputActionValue(static_cast<float>(Value)),
							TArray<UInputModifier*>(), TArray<UInputTrigger*>());
						bInjected = true;
					}
				}
			}
			break;
		}
	}

	if (!bInjected)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No PIE EnhancedInput subsystem"), EMCPErrorCode::InvalidArgument,
			TEXT("Could not reach the PIE player's UEnhancedInputLocalPlayerSubsystem (no local player / not Enhanced Input). Ensure a possessed player using Enhanced Input."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("action"), ActionPath);
	Result->SetNumberField(TEXT("value"), Value);
	Result->SetBoolField(TEXT("injected"), true);
	return Result;
}

TSharedPtr<FJsonObject> FMCPAutomationCommands::HandleSendMouseInput(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing params"),
			EMCPErrorCode::InvalidArgument,
			TEXT("This handler requires a params object. Pass a JSON object with at minimum the documented required fields for this tool."));
	}

	double X = 0.0, Y = 0.0;
	if (!Params->TryGetNumberField(TEXT("x"), X) || !Params->TryGetNumberField(TEXT("y"), Y))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'x' and/or 'y' parameters"),
			EMCPErrorCode::InvalidArgument,
			TEXT("Both `x` and `y` are required (numeric, screen-space pixel coordinates). The origin is the top-left of the primary monitor (Slate convention). Use `take_screenshot` to verify a click target's screen position first."));
	}

	FString EventTypeStr = TEXT("move");
	Params->TryGetStringField(TEXT("event_type"), EventTypeStr);

	FString ButtonStr = TEXT("left");
	Params->TryGetStringField(TEXT("button"), ButtonStr);

	FKey MouseButton = EKeys::LeftMouseButton;
	if (ButtonStr == TEXT("right"))
	{
		MouseButton = EKeys::RightMouseButton;
	}
	else if (ButtonStr == TEXT("middle"))
	{
		MouseButton = EKeys::MiddleMouseButton;
	}

	const FVector2D ScreenPos(X, Y);
	const FModifierKeysState EmptyModifiers;

	if (EventTypeStr == TEXT("move"))
	{
		FPointerEvent MoveEvent(
			0,               // PointerIndex
			ScreenPos,       // ScreenSpacePosition
			ScreenPos,       // LastScreenSpacePosition
			TSet<FKey>(),    // PressedButtons
			EKeys::Invalid,  // EffectingButton
			0.0f,            // WheelDelta
			EmptyModifiers   // ModifierKeys
		);
		FSlateApplication::Get().ProcessMouseMoveEvent(MoveEvent);
	}
	else if (EventTypeStr == TEXT("pressed"))
	{
		TSet<FKey> PressedButtons;
		PressedButtons.Add(MouseButton);

		FPointerEvent DownEvent(
			0,
			ScreenPos,
			ScreenPos,
			PressedButtons,
			MouseButton,
			0.0f,
			EmptyModifiers
		);
		FSlateApplication::Get().ProcessMouseButtonDownEvent(nullptr, DownEvent);
	}
	else if (EventTypeStr == TEXT("released"))
	{
		FPointerEvent UpEvent(
			0,
			ScreenPos,
			ScreenPos,
			TSet<FKey>(),
			MouseButton,
			0.0f,
			EmptyModifiers
		);
		FSlateApplication::Get().ProcessMouseButtonUpEvent(UpEvent);
	}
	else
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Invalid event_type: %s. Expected: move, pressed, released"), *EventTypeStr),
			EMCPErrorCode::InvalidArgument,
			TEXT("`event_type` for send_mouse_input must be exactly `move`, `pressed`, or `released` (case-sensitive). To simulate a click at (x,y), send `pressed` then `released` — both at the same coordinates."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("x"), X);
	Result->SetNumberField(TEXT("y"), Y);
	Result->SetStringField(TEXT("event_type"), EventTypeStr);
	Result->SetStringField(TEXT("button"), ButtonStr);
	return Result;
}

// ---------------------------------------------------------------------------
// Console Command
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMCPAutomationCommands::HandleExecuteConsoleCommand(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing params"),
			EMCPErrorCode::InvalidArgument,
			TEXT("This handler requires a params object. Pass a JSON object with at minimum the documented required fields for this tool."));
	}

	FString Command;
	if (!Params->TryGetStringField(TEXT("command"), Command))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'command' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`command` is required (string, an Unreal console command). Examples: `stat fps`, `r.ScreenPercentage 75`, `py print('hi')`. Multi-line Python: pass `py\\n<code>` and the multi-line code will be written to a temp .py file. Note: `exit` / `quit` are blocked."));
	}

	// Convenience guard (NOT a security boundary — the console/py escape hatch can
	// still terminate the process by other means). Reject the bare `exit` / `quit`
	// verbs, which would close the editor and sever the MCP connection. Match the
	// command VERB (first whitespace-delimited token) rather than a substring, so we
	// don't false-positive on arguments or py code that merely contain the letters
	// (e.g. `py print("exit")`, a cvar ending in "quit").
	FString TrimmedCommand = Command.TrimStartAndEnd();
	FString CommandVerb = TrimmedCommand;
	int32 SpaceIdx = INDEX_NONE;
	if (TrimmedCommand.FindChar(TEXT(' '), SpaceIdx))
	{
		CommandVerb = TrimmedCommand.Left(SpaceIdx);
	}
	CommandVerb.ToLowerInline();
	if (CommandVerb == TEXT("exit") || CommandVerb == TEXT("quit"))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Blocked: exit/quit commands are not allowed via MCP"),
			EMCPErrorCode::FeatureDisabled,
			TEXT("The `exit` / `quit` console verbs are blocked because they would terminate the editor and sever the MCP connection. Use the editor UI to close the application if needed."));
	}

	// Route to PIE world if active, otherwise editor world
	UWorld* TargetWorld = nullptr;
	bool bPIEActive = false;

	if (GEditor && GEditor->IsPlaySessionInProgress())
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World())
			{
				TargetWorld = Context.World();
				bPIEActive = true;
				break;
			}
		}
	}

	if (!TargetWorld && GEditor)
	{
		TargetWorld = GEditor->GetEditorWorldContext().World();
	}

	if (!TargetWorld)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No world available"),
			EMCPErrorCode::Internal,
			TEXT("Could not resolve a UWorld to execute the console command against — checked PIE world (if active) and GEditor's editor-world context. This typically means the editor is mid-startup or shutdown. Retry after the editor is fully loaded."));
	}

	FStringOutputDevice OutputDevice;

	// Python commands always go through the stdout-capture wrapper. The Python
	// plugin redirects sys.stdout to its LogPython category, not to the
	// caller's FStringOutputDevice — bare `GEngine->Exec(world, "py ...", od)`
	// runs the code but `od` stays empty. Wrapping the code in our own
	// stdout/stderr redirect + temp-file dump is the only way to surface
	// `print()` output to the MCP caller. Cost is ~200µs/call for two
	// temp-file writes + reads; cheap enough to apply uniformly.
	FString TempPyFile;       // Wrapper script that captures stdout/stderr.
	FString TempUserFile;     // User's raw Python code (referenced by the wrapper).
	FString TempOutFile;      // File Python writes its captured stdout/stderr to.
	const bool bIsPython = Command.StartsWith(TEXT("py ")) ||
	                       Command.StartsWith(TEXT("py\n")) ||
	                       Command.StartsWith(TEXT("py\r"));

	if (bIsPython)
	{
		// Strip the `py` prefix and any leading whitespace/newline to get the
		// raw user code. Works for both single-line (`py print('hi')`) and
		// multi-line (`py\n<body>`) forms — both go through the same wrapper.
		FString PythonCode = Command.Mid(2);  // drop "py"
		PythonCode.TrimStartInline();         // drop the separator space/CR/LF

		TempPyFile = FPaths::CreateTempFilename(
			*FPaths::ProjectSavedDir(), TEXT("MCP_"), TEXT(".py"));
		// Sibling files — keep them next to TempPyFile so cleanup is uniform.
		// User code lives in TempUserFile (raw, no wrapping → no escaping concerns).
		// Wrapper in TempPyFile redirects stdout/stderr, execs user file, dumps capture.
		TempUserFile = TempPyFile.Replace(TEXT(".py"), TEXT(".user.py"));
		TempOutFile  = TempPyFile.Replace(TEXT(".py"), TEXT(".out"));

		// UTF-8 without BOM — Python's open(..., encoding='utf-8') rejects UTF-16
		// and UE's default encoding for SaveStringToFile is UTF-16.
		FFileHelper::SaveStringToFile(PythonCode, *TempUserFile,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

		// Use forward slashes — valid on Windows and avoids backslash-escape
		// hazards in the embedded Python r'...' literal.
		FString EscapedUserPath = TempUserFile.Replace(TEXT("\\"), TEXT("/"));
		FString EscapedOutPath  = TempOutFile .Replace(TEXT("\\"), TEXT("/"));

		FString WrappedCode = FString::Printf(TEXT(
			"import sys as _mcp_sys, io as _mcp_io, traceback as _mcp_tb\n"
			"_mcp_stdout = _mcp_io.StringIO()\n"
			"_mcp_stderr = _mcp_io.StringIO()\n"
			"_mcp_orig_stdout = _mcp_sys.stdout\n"
			"_mcp_orig_stderr = _mcp_sys.stderr\n"
			"_mcp_sys.stdout = _mcp_stdout\n"
			"_mcp_sys.stderr = _mcp_stderr\n"
			"try:\n"
			"    with open(r'%s', 'r', encoding='utf-8') as _mcp_uf:\n"
			"        _mcp_src = _mcp_uf.read()\n"
			"    exec(compile(_mcp_src, r'%s', 'exec'), {'__name__': '__main__'})\n"
			"except Exception:\n"
			"    _mcp_tb.print_exc(file=_mcp_stderr)\n"
			"finally:\n"
			"    _mcp_sys.stdout = _mcp_orig_stdout\n"
			"    _mcp_sys.stderr = _mcp_orig_stderr\n"
			"    try:\n"
			"        with open(r'%s', 'w', encoding='utf-8') as _mcp_f:\n"
			"            _mcp_f.write(_mcp_stdout.getvalue())\n"
			"            _err = _mcp_stderr.getvalue()\n"
			"            if _err:\n"
			"                _mcp_f.write('\\n[stderr]\\n')\n"
			"                _mcp_f.write(_err)\n"
			"    except Exception:\n"
			"        pass\n"),
			*EscapedUserPath,
			*EscapedUserPath,
			*EscapedOutPath);

		FFileHelper::SaveStringToFile(WrappedCode, *TempPyFile,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FString PyExecCmd = FString::Printf(TEXT("py \"%s\""), *TempPyFile);
		GEngine->Exec(TargetWorld, *PyExecCmd, OutputDevice);
	}
	else
	{
		// Non-Python console commands route normally through GEngine->Exec
		// which dispatches via IConsoleManager.
		GEngine->Exec(TargetWorld, *Command, OutputDevice);
	}

	FString Output = OutputDevice;

	// If we captured Python stdout to a sibling file, append it to the output.
	if (!TempOutFile.IsEmpty() && IFileManager::Get().FileExists(*TempOutFile))
	{
		FString PyCaptured;
		if (FFileHelper::LoadFileToString(PyCaptured, *TempOutFile))
		{
			if (!PyCaptured.IsEmpty())
			{
				if (!Output.IsEmpty()) Output += TEXT("\n");
				Output += PyCaptured;
			}
		}
		IFileManager::Get().Delete(*TempOutFile);
	}

	// Clean up temp files
	if (!TempPyFile.IsEmpty())
	{
		IFileManager::Get().Delete(*TempPyFile);
	}
	if (!TempUserFile.IsEmpty())
	{
		IFileManager::Get().Delete(*TempUserFile);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("command"), Command);
	Result->SetStringField(TEXT("output"), Output);
	Result->SetBoolField(TEXT("pie_active"), bPIEActive);
	return Result;
}

// ---------------------------------------------------------------------------
// Live Coding Compile  (free function — called on game thread by Bridge)
// ---------------------------------------------------------------------------

static void ResolveLiveCoding(TSharedPtr<TPromise<FString>> Promise,
                              const FString& Status, const FString& MsgKey, const FString& MsgVal)
{
	TSharedPtr<FJsonObject> Outer = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
	Inner->SetBoolField(TEXT("success"), Status == TEXT("success"));
	Inner->SetStringField(MsgKey, MsgVal);
	Outer->SetStringField(TEXT("status"), Status);
	Outer->SetObjectField(TEXT("result"), Inner);

	FString Json;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Outer.ToSharedRef(), Writer);
	Promise->SetValue(Json);
}

void AutomationCommands_LiveCodingStart(TSharedPtr<TPromise<FString>> Promise, FMCPLogCollector* LogCollector)
{
	check(IsInGameThread());

	// Mirror FLevelEditorActionCallbacks::RecompileGameCode_Clicked
	// (LevelEditorActions.cpp:1339, the Ctrl+Alt+P "Recompile Game Code" toolbar / Ctrl+Alt+F11
	// hot-reload hotkey path) exactly. The user-triggered path uses the no-arg async Compile()
	// overload — never the WaitForCompletion overload — and lets Live Coding's own Tick on
	// FCoreDelegates::OnEndFrame drive the compile to completion in the background. This keeps
	// the editor responsive (no FScopedSlowTask::MakeDialog() blocking the game thread) and
	// avoids the modal "Compiling..." dialog that makes every compile look like a full rebuild.

	if (!FModuleManager::Get().IsModuleLoaded(TEXT("LiveCoding")))
	{
		if (LogCollector) LogCollector->InjectEvent(TEXT("LIVECODING"), TEXT("Compile"), ELogVerbosity::Error, TEXT("LiveCoding module is not loaded"));
		ResolveLiveCoding(Promise, TEXT("error"), TEXT("error"),
			TEXT("LiveCoding module is not loaded"));
		return;
	}

	ILiveCodingModule& LiveCoding = FModuleManager::LoadModuleChecked<ILiveCodingModule>(TEXT("LiveCoding"));

	if (!LiveCoding.IsEnabledByDefault())
	{
		if (LogCollector) LogCollector->InjectEvent(TEXT("LIVECODING"), TEXT("Compile"), ELogVerbosity::Error, TEXT("Live Coding is disabled in editor preferences"));
		ResolveLiveCoding(Promise, TEXT("error"), TEXT("error"),
			TEXT("Live Coding is disabled in editor preferences (Editor Preferences > General > Live Coding > Enable Live Coding)"));
		return;
	}

	LiveCoding.EnableForSession(true);

	if (!LiveCoding.IsEnabledForSession())
	{
		const FText EnableErr = LiveCoding.GetEnableErrorText();
		const FString Msg = EnableErr.IsEmpty()
			? FString(TEXT("Live Coding could not be enabled for this session — close the editor and rebuild from your IDE before retrying"))
			: EnableErr.ToString();
		if (LogCollector) LogCollector->InjectEvent(TEXT("LIVECODING"), TEXT("Compile"), ELogVerbosity::Error, *Msg);
		ResolveLiveCoding(Promise, TEXT("error"), TEXT("error"), Msg);
		return;
	}

	if (LiveCoding.IsCompiling())
	{
		if (LogCollector) LogCollector->InjectEvent(TEXT("LIVECODING"), TEXT("Compile"), ELogVerbosity::Warning, TEXT("Compile already in progress"));
		ResolveLiveCoding(Promise, TEXT("error"), TEXT("error"),
			TEXT("A Live Coding compile is already in progress"));
		return;
	}

	if (LogCollector) LogCollector->InjectEvent(TEXT("LIVECODING"), TEXT("Compile"), ELogVerbosity::Display, TEXT("Compile scheduled for end-of-frame (WaitForCompletion deferred to the safe reinstancing point)"));

	// We MUST distinguish Success / NoChanges / Failure / Cancelled. UE 5.7's
	// public async API (Compile() + GetOnPatchCompleteDelegate) cannot do this:
	// the patch-complete delegate carries no payload and fires ONLY on
	// Success-with-changes (LiveCodingModule.cpp:908), so a hard C++ compile
	// FAILURE is indistinguishable from a clean no-op — which is exactly the bug
	// where a failed compile reported success. The ONLY API that returns the true
	// ELiveCodingCompileResult is the synchronous WaitForCompletion overload
	// (ILiveCodingModule.h:62).
	//
	// CRASH FIX (2026-06-26): WaitForCompletion REINSTANCES changed UObjects inline. Calling it directly
	// here — inside the bridge's game-thread AsyncTask, mid command-dispatch — collided with an active
	// UObject-hash iteration and crashed the editor ("Trying to modify UObject map (FindOrAdd) that is
	// currently being iterated", UObjectHash.cpp). UE's own Live Coding reinstances on OnEndFrame, so we
	// DEFER the WaitForCompletion compile to a one-shot OnEndFrame callback: the reinstancing then runs at
	// that safe frame boundary while we STILL get the honest result (which the async no-arg Compile()
	// cannot report). The server thread stays blocked on the future (600s cap) and the game thread keeps
	// ticking until OnEndFrame fires, so this can never deadlock. GIsRunningUnattendedScript (set inside
	// the callback) suppresses the FScopedSlowTask "Compiling..." modal. On failure the exact `error C....`
	// text lives only in the LiveCodingConsole window / LogLiveCoding channel, so we point the caller there.
	TSharedPtr<FDelegateHandle> EndFrameHandle = MakeShared<FDelegateHandle>();
	*EndFrameHandle = FCoreDelegates::OnEndFrame.AddLambda([Promise, LogCollector, EndFrameHandle]()
	{
		// CRITICAL: copy every capture we need to the STACK before the long WaitForCompletion below.
		// WaitForCompletion pumps the game thread, which fires RE-ENTRANT OnEndFrame broadcasts; those can
		// compact this multicast delegate's invocation array and FREE this functor's capture storage WHILE
		// we are still executing inside it. Touching a capture (e.g. LogCollector/Promise) after that frees
		// faults (observed: crash at the InjectEvent on the success path, right after "Live coding succeeded").
		// Locals live on our stack and survive; the TSharedPtr copy keeps the promise alive. Remove() nulls
		// our slot immediately, so the re-entrant broadcasts never re-invoke this lambda.
		TSharedPtr<TPromise<FString>> P = Promise;
		FMCPLogCollector* LC = LogCollector;
		FCoreDelegates::OnEndFrame.Remove(*EndFrameHandle);

		ILiveCodingModule& LiveCoding = FModuleManager::LoadModuleChecked<ILiveCodingModule>(TEXT("LiveCoding"));
		TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);

		ELiveCodingCompileResult CompileResult = ELiveCodingCompileResult::Failure;
		LiveCoding.Compile(ELiveCodingCompileFlags::WaitForCompletion, &CompileResult);

		switch (CompileResult)
		{
		case ELiveCodingCompileResult::Success:
			if (LC) LC->InjectEvent(TEXT("LIVECODING"), TEXT("Compile"), ELogVerbosity::Display, TEXT("Compile succeeded — patches applied"));
			ResolveLiveCoding(P, TEXT("success"), TEXT("message"),
				TEXT("Live Coding compile succeeded — patches applied"));
			break;
		case ELiveCodingCompileResult::NoChanges:
			if (LC) LC->InjectEvent(TEXT("LIVECODING"), TEXT("Compile"), ELogVerbosity::Display, TEXT("Compile succeeded — no code changes detected"));
			ResolveLiveCoding(P, TEXT("success"), TEXT("message"),
				TEXT("Live Coding compile succeeded — no code changes detected (nothing to patch)"));
			break;
		case ELiveCodingCompileResult::Failure:
			if (LC) LC->InjectEvent(TEXT("LIVECODING"), TEXT("Compile"), ELogVerbosity::Error, TEXT("Compile FAILED — C++ error(s)"));
			ResolveLiveCoding(P, TEXT("error"), TEXT("error"),
				TEXT("Live Coding compile FAILED — the C++ did not compile. The exact `error C....` text is NOT exposed by the Live Coding API; read it from the LiveCodingConsole window, or from the editor log's LogLiveCoding channel. Fix the error and retry, or do a full build.sh for the complete UBT error output."));
			break;
		case ELiveCodingCompileResult::Cancelled:
			if (LC) LC->InjectEvent(TEXT("LIVECODING"), TEXT("Compile"), ELogVerbosity::Warning, TEXT("Compile cancelled"));
			ResolveLiveCoding(P, TEXT("error"), TEXT("error"),
				TEXT("Live Coding compile was cancelled before completing. Retry."));
			break;
		case ELiveCodingCompileResult::CompileStillActive:
			ResolveLiveCoding(P, TEXT("error"), TEXT("error"),
				TEXT("A prior Live Coding compile is still active. Wait for it to finish and retry."));
			break;
		default: // InProgress (shouldn't occur with WaitForCompletion) / NotStarted / future values
			if (LC) LC->InjectEvent(TEXT("LIVECODING"), TEXT("Compile"), ELogVerbosity::Error, TEXT("Compile returned an unexpected result"));
			ResolveLiveCoding(P, TEXT("error"), TEXT("error"),
				TEXT("Live Coding compile returned an unexpected result (not Success/NoChanges/Failure/Cancelled). Check the LogLiveCoding channel."));
			break;
		}
	});
}
