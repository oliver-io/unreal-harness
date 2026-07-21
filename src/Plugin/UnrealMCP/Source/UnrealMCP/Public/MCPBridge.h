#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "HAL/ThreadSafeBool.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Http.h"
#include "Json.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Commands/MCPEditorCommands.h"
#include "Commands/MCPBlueprintCommands.h"
#include "Commands/MCPBlueprintGraphCommands.h"
#include "Commands/MCPMaterialCommands.h"
#include "Commands/MCPAutomationCommands.h"
#include "Commands/MCPRecorderCommands.h"
#include "Commands/MCPNiagaraCommands.h"
#include "Commands/MCPPCGCommands.h"
#include "Commands/MCPStateTreeCommands.h"
#include "Commands/MCPEQSCommands.h"
#include "Commands/MCPAIRuntimeCommands.h"
#include "Commands/MCPAnimationCommands.h"
#include "Commands/MCPSkeletalMeshCommands.h"
#include "Commands/MCPMeshCommands.h"
#include "Commands/MCPTextureCommands.h"
#include "Commands/MCPMeshImportCommands.h"
#include "Commands/MCPSoundImportCommands.h"
#include "Commands/MCPFontImportCommands.h"
#include "Commands/MCPLevelCommands.h"
#include "Commands/MCPDataAssetCommands.h"
#include "Commands/MCPReflectionCommands.h"
#include "Commands/MCPInspectionCommands.h"
#include "Commands/MCPSceneCommands.h"
#include "Commands/MCPLandscapeCommands.h"
#include "Commands/MCPFoliageCommands.h"
#include "Commands/MCPAssetFactoryCommands.h"
#include "Commands/MCPWidgetCommands.h"
#include "Commands/MCPIKCommands.h"
#include "Commands/MCPGameplayTagCommands.h"
#include "Commands/MCPGASCommands.h"
#include "Commands/MCPDiagnosticsCommands.h"
#include "Commands/MCPBlueprintPolishCommands.h"
#include "Commands/MCPKinematicsCommands.h"
#include "Commands/MCPStreamingCommands.h"
#include "MCPLogCollector.h"
#include "MCPBridge.generated.h"

class FMCPServerRunnable;

/**
 * Editor subsystem for MCP Bridge
 * Handles communication between external tools and the Unreal Editor
 * through a TCP socket connection. Commands are received as JSON and
 * routed to appropriate command handlers.
 */
UCLASS()
class UNREALMCP_API UMCPBridge : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	UMCPBridge();
	virtual ~UMCPBridge();

	// UEditorSubsystem implementation
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// Server functions
	void StartServer();
	void StopServer();
	bool IsRunning() const { return bIsRunning; }

	// Command execution
	FString ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

	/** Unified log collector — captures all UE_LOG + editor events to MCP_Unified.log */
	FMCPLogCollector* GetLogCollector() const { return LogCollector.Get(); }

	/**
	 * Game-thread continuation: record a finished Live Coding compile result and
	 * flip the phase to Done so the next poll (editor_live_coding_compile poll:true)
	 * can collect it. Thread-safe (takes LiveCodingCS). See GAP-060.
	 */
	void FinishLiveCoding(const FString& InResult);

	/**
	 * Cached Pixel Streaming state (portable.dev#19 M2). mcp_status answers
	 * synchronously on the NETWORK thread and must not query the PS2 modules
	 * (game-thread shaped) — it reads this cache instead. Written on the game
	 * thread by the stream_* handlers (FMCPStreamingCommands) and by the editor
	 * streamer's OnStreamingStarted/Stopped delegates; both accessors take
	 * StreamStateCS, so they are safe from any thread.
	 */
	void SetStreamState(const FMCPStreamState& InState);
	FMCPStreamState GetStreamState() const;

private:
	// Server state
	bool bIsRunning;

	/**
	 * Boot gate. False until FEditorDelegates::OnEditorInitialized fires (editor
	 * "fully initialized" — UnrealEdMisc.cpp). The TCP listener binds during this
	 * subsystem's Initialize(), i.e. mid UEditorEngine::Init, so connections are
	 * accepted long before the editor is interactive. ExecuteCommand reads this on
	 * the server thread and refuses to dispatch any game-thread work until it is
	 * true — that is what prevents the queued-command-during-init startup crash
	 * (see docs/bugs/mcp.md). Written on the game thread by the delegate; read on
	 * the server thread; hence atomic.
	 */
	FThreadSafeBool bEditorInteractive;
	FDelegateHandle EditorInitializedHandle;

	/**
	 * PIE gate. True while a Play-In-Editor (or Simulate-In-Editor) session is
	 * running. Driven on the game thread by FEditorDelegates::BeginPIE/EndPIE,
	 * read on the server thread by ExecuteCommand (hence atomic, same rationale as
	 * bEditorInteractive). While true, asset-mutating/loading commands
	 * (IsBlockedDuringPie) are refused with EMCPErrorCode::PieActive instead of
	 * being dispatched into a game thread where UEditorAssetLibrary::LoadAsset
	 * returns null — see docs/bugs/mcp.md.
	 */
	FThreadSafeBool bPlayInEditorActive;
	FDelegateHandle BeginPIEHandle;
	FDelegateHandle EndPIEHandle;

	/**
	 * Live Coding async state (GAP-060). A Live Coding compile blocks the GAME
	 * thread for its whole duration (ILiveCodingModule::Compile, WaitForCompletion).
	 * Pre-GAP-060 the SERVER thread ALSO blocked, waiting on the compile future —
	 * and because the single server thread both accepts connections and runs each
	 * command inline (FMCPServerRunnable::Run) while the TS client is
	 * connection-per-command, that wait starved EVERY other command (even
	 * mcp_status, which needs no game thread) to a socket timeout: the editor
	 * looked crashed. Now the handler kicks the compile, returns a
	 * `live_coding_in_progress` ticket immediately, and the server thread stays
	 * free to answer mcp_status/ping. The caller polls mcp_status until the compile
	 * clears, then retrieves the stored result (editor_live_coding_compile poll:true).
	 *
	 * Phase/Result are shared between the server thread (the command handler) and
	 * the game thread (the compile-completion continuation FinishLiveCoding), so
	 * every access goes through LiveCodingCS.
	 */
	enum class ELiveCodingPhase : uint8 { Idle, Compiling, Done };
	mutable FCriticalSection LiveCodingCS;
	ELiveCodingPhase LiveCodingPhase = ELiveCodingPhase::Idle;
	FString LiveCodingResult;  // serialized envelope string; valid only when Phase==Done

	/** Pixel Streaming state cache (see Set/GetStreamState above). */
	mutable FCriticalSection StreamStateCS;
	FMCPStreamState StreamState;

	TSharedPtr<FSocket> ListenerSocket;
	TSharedPtr<FSocket> ConnectionSocket;
	FRunnableThread* ServerThread;

	// Server configuration
	FIPv4Address ServerAddress;
	uint16 Port;

	// Command handler instances
	TSharedPtr<FMCPEditorCommands> EditorCommands;
	TSharedPtr<FMCPBlueprintCommands> BlueprintCommands;
	TSharedPtr<FMCPBlueprintGraphCommands> BlueprintGraphCommands;
	TSharedPtr<FMCPMaterialCommands> MaterialCommands;
	TSharedPtr<FMCPAutomationCommands> AutomationCommands;
	TSharedPtr<FMCPRecorderCommands> RecorderCommands;
	TSharedPtr<FMCPNiagaraCommands> NiagaraCommands;
	TSharedPtr<FMCPPCGCommands> PCGCommands;
	TSharedPtr<FMCPStateTreeCommands> StateTreeCommands;
	TSharedPtr<FMCPEQSCommands> EQSCommands;
	TSharedPtr<FMCPAIRuntimeCommands> AIRuntimeCommands;
	TSharedPtr<FMCPAnimationCommands> AnimationCommands;
	TSharedPtr<FMCPSkeletalMeshCommands> SkeletalMeshCommands;
	TSharedPtr<FMCPMeshCommands> MeshCommands;
	TSharedPtr<FMCPTextureCommands> TextureCommands;
	TSharedPtr<FMCPMeshImportCommands> MeshImportCommands;
	TSharedPtr<FMCPSoundImportCommands> SoundImportCommands;
	TSharedPtr<FMCPFontImportCommands> FontImportCommands;
	TSharedPtr<FMCPLevelCommands> LevelCommands;
	TSharedPtr<FMCPDataAssetCommands> DataAssetCommands;
	TSharedPtr<FMCPReflectionCommands> ReflectionCommands;
	TSharedPtr<FMCPInspectionCommands> InspectionCommands;
	TSharedPtr<FMCPSceneCommands> SceneCommands;
	TSharedPtr<FMCPLandscapeCommands> LandscapeCommands;
	TSharedPtr<FMCPFoliageCommands> FoliageCommands;
	TSharedPtr<FMCPAssetFactoryCommands> AssetFactoryCommands;
	TSharedPtr<FMCPWidgetCommands> WidgetCommands;
	TSharedPtr<FMCPIKCommands> IKCommands;
	TSharedPtr<FMCPGameplayTagCommands> GameplayTagCommands;
	TSharedPtr<FMCPGASCommands> GASCommands;
	TSharedPtr<FMCPDiagnosticsCommands> DiagnosticsCommands;
	TSharedPtr<FMCPBlueprintPolishCommands> BlueprintPolishCommands;
	TSharedPtr<FMCPKinematicsCommands> KinematicsCommands;
	TSharedPtr<FMCPStreamingCommands> StreamingCommands;

	// Unified log collector
	TUniquePtr<FMCPLogCollector> LogCollector;
};