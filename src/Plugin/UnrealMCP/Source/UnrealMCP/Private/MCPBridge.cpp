#include "MCPBridge.h"
#include "MCPServerRunnable.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "JsonObjectConverter.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Kismet/GameplayStatics.h"
#include "Async/Async.h"
#include "Misc/ScopeLock.h" // FScopeLock — guards the Live Coding async state (GAP-060)
#include "Containers/Ticker.h" // FTSTicker — top-level game-thread dispatch for heavy import/build ops (GAP-057)
#include "HAL/FileManager.h" // IFileManager::FileExists — server-thread screenshot file poll (GAP-007)
#include "HAL/PlatformProcess.h" // FPlatformProcess::Sleep — bounded server-thread poll (GAP-007)
// Add Blueprint related includes
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/BlueprintFactory.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
// UE5.5 correct includes
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UObject/Field.h"
#include "UObject/FieldPath.h"
// Blueprint Graph specific includes
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_CallFunction.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "GameFramework/InputSettings.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Editor.h" // FEditorDelegates::OnEditorInitialized + BeginPIE/EndPIE — gate signals
#include "Editor/EditorEngine.h" // GEditor->IsPlaySessionInProgress() (PIE gate seed)
// Include our new command handler classes
#include "Commands/MCPEditorCommands.h"
#include "Commands/MCPBlueprintCommands.h"
#include "Commands/MCPBlueprintGraphCommands.h"
#include "Commands/MCPCommonUtils.h"
#include "Commands/AssetManager.h"

// Default settings
#define MCP_SERVER_HOST "127.0.0.1"
#define MCP_SERVER_PORT 55557

namespace
{
    // Serialize a JSON object to a compact wire string (matches the writer the
    // game-thread dispatch path uses for its responses).
    FString MCPSerializeJson(const TSharedRef<FJsonObject>& Obj)
    {
        FString Out;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Obj, Writer);
        return Out;
    }

    // Wrap an inner CreateErrorResponse result ({success:false, error,
    // error_code, error_hint}) into the bridge's {status:"error", ...} envelope,
    // byte-for-byte matching the game-thread error path (MCPBridge.cpp
    // bSuccess==false branch). Used by the boot gate, which must respond on the
    // SERVER thread without queuing any game-thread work.
    FString MCPWrapErrorEnvelope(const TSharedPtr<FJsonObject>& InnerResult)
    {
        TSharedRef<FJsonObject> Env = MakeShared<FJsonObject>();
        Env->SetStringField(TEXT("status"), TEXT("error"));

        FString Msg, Code, Hint;
        if (InnerResult->HasField(TEXT("error")))      { Msg  = InnerResult->GetStringField(TEXT("error")); }
        if (InnerResult->HasField(TEXT("error_code"))) { Code = InnerResult->GetStringField(TEXT("error_code")); }
        if (InnerResult->HasField(TEXT("error_hint"))) { Hint = InnerResult->GetStringField(TEXT("error_hint")); }

        Env->SetStringField(TEXT("error"), Msg);
        if (!Code.IsEmpty()) { Env->SetStringField(TEXT("error_code"), Code); }
        if (!Hint.IsEmpty()) { Env->SetStringField(TEXT("error_hint"), Hint); }
        Env->SetObjectField(TEXT("result"), InnerResult);
        return MCPSerializeJson(Env);
    }

    // GAP-060: the immediate "compile started / still compiling" ticket returned by
    // editor_live_coding_compile. The real result is collected later via a poll:true
    // retrieval once mcp_status reports the compile has cleared.
    FString MCPLiveCodingTicket()
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("live_coding_in_progress"), true);
        Result->SetStringField(TEXT("phase"), TEXT("compiling"));
        Result->SetStringField(TEXT("message"),
            TEXT("Live Coding compile is in progress on the editor game thread. The bridge ")
            TEXT("stays responsive: mcp_status answers with result.live_coding_in_progress=true. ")
            TEXT("Poll mcp_status until that clears, then re-issue editor_live_coding_compile ")
            TEXT("with poll:true to retrieve the compile result."));

        TSharedRef<FJsonObject> Env = MakeShared<FJsonObject>();
        Env->SetStringField(TEXT("status"), TEXT("success"));
        Env->SetObjectField(TEXT("result"), Result);
        return MCPSerializeJson(Env);
    }

    // GAP-007: confirm an editor_screenshot's async (end-of-frame) capture actually
    // produced a file, on the SERVER thread.
    //
    // editor_screenshot's editor-viewport path kicks FScreenshotRequest, which is
    // serviced at end-of-frame inside FViewport::Draw — the handler can't confirm the
    // file synchronously, and it MUST NOT block the game thread to wait, because the
    // game thread is the very thing that renders the frame the request needs (polling
    // there self-deadlocks until timeout). So the handler returns result.status ==
    // "requested" and we poll for the output file HERE, on the server thread, where
    // blocking leaves the game thread free to render the invalidated frame.
    //
    // Takes the wrapped envelope string the dispatch tail is about to return; if it's
    // a successful screenshot still in the "requested" state, polls result.path with a
    // bounded budget and rewrites the envelope to an honest success("captured") or a
    // `timeout` error — never the old false "requested". Any other envelope (sync
    // capture already "captured", an error, an unparseable/foreign shape) passes
    // through untouched.
    FString MCPAwaitScreenshotFile(const FString& Envelope)
    {
        // Bounded budget. Comfortably under the client's 30s default recv timeout
        // (connection.ts) once game-thread dispatch + transport are accounted for. A
        // forced-realtime viewport renders in well under a second; the budget exists
        // to fail fast and HONESTLY when the window is occluded/minimized and never
        // composites a frame, rather than to wait out a multi-minute render cadence.
        const double BudgetSeconds = 12.0;
        const float PollIntervalSeconds = 0.1f;

        TSharedPtr<FJsonObject> Root;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Envelope);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            return Envelope; // not parseable — leave it exactly as produced
        }

        FString TopStatus;
        if (!Root->TryGetStringField(TEXT("status"), TopStatus) || TopStatus != TEXT("success"))
        {
            return Envelope; // already an error envelope — don't touch it
        }

        const TSharedPtr<FJsonObject>* ResultObj = nullptr;
        if (!Root->TryGetObjectField(TEXT("result"), ResultObj) || !ResultObj || !ResultObj->IsValid())
        {
            return Envelope;
        }

        FString CaptureStatus;
        (*ResultObj)->TryGetStringField(TEXT("status"), CaptureStatus);
        if (CaptureStatus != TEXT("requested"))
        {
            return Envelope; // sync path already wrote the file (status == "captured")
        }

        FString Path;
        (*ResultObj)->TryGetStringField(TEXT("path"), Path);
        if (Path.IsEmpty())
        {
            return Envelope;
        }

        const double Start = FPlatformTime::Seconds();
        bool bFileAppeared = false;
        while (FPlatformTime::Seconds() - Start < BudgetSeconds)
        {
            if (IFileManager::Get().FileExists(*Path))
            {
                // The screenshot writer runs on the game thread after the render
                // readback resolves; give it a brief settle so we never report a
                // half-flushed file, then accept.
                FPlatformProcess::Sleep(PollIntervalSeconds);
                bFileAppeared = true;
                break;
            }
            FPlatformProcess::Sleep(PollIntervalSeconds);
        }

        if (bFileAppeared)
        {
            (*ResultObj)->SetStringField(TEXT("status"), TEXT("captured"));
            (*ResultObj)->SetStringField(TEXT("message"),
                TEXT("Screenshot rendered and written (editor viewport forced to render; ")
                TEXT("file confirmed on the server thread)."));
            return MCPSerializeJson(Root.ToSharedRef());
        }

        // Honest timeout — the editor viewport never rendered a qualifying frame.
        TSharedPtr<FJsonObject> InnerErr = FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(
                TEXT("Screenshot was requested but no file appeared at '%s' within %.0fs. ")
                TEXT("The editor viewport did not render a frame to service the capture."),
                *Path, BudgetSeconds),
            EMCPErrorCode::Timeout,
            TEXT("The editor level viewport could not composite a frame — almost always because ")
            TEXT("the editor window is occluded, minimized, or backgrounded with 'Use Less CPU ")
            TEXT("when in Background' on. Foreground the editor (and disable that setting), then ")
            TEXT("retry. To capture the whole window regardless, use mode=\"editor\" (synchronous, ")
            TEXT("OS-level), or capture from a PIE session."));
        return MCPWrapErrorEnvelope(InnerErr);
    }
}

UMCPBridge::UMCPBridge()
{
    EditorCommands = MakeShared<FMCPEditorCommands>();
    BlueprintCommands = MakeShared<FMCPBlueprintCommands>();
    BlueprintGraphCommands = MakeShared<FMCPBlueprintGraphCommands>();
    MaterialCommands = MakeShared<FMCPMaterialCommands>();
    AutomationCommands = MakeShared<FMCPAutomationCommands>();
    RecorderCommands = MakeShared<FMCPRecorderCommands>();
    NiagaraCommands = MakeShared<FMCPNiagaraCommands>();
    PCGCommands = MakeShared<FMCPPCGCommands>();
    StateTreeCommands = MakeShared<FMCPStateTreeCommands>();
    EQSCommands = MakeShared<FMCPEQSCommands>();
    AIRuntimeCommands = MakeShared<FMCPAIRuntimeCommands>();
    AnimationCommands = MakeShared<FMCPAnimationCommands>();
    SkeletalMeshCommands = MakeShared<FMCPSkeletalMeshCommands>();
    MeshCommands = MakeShared<FMCPMeshCommands>();
    TextureCommands = MakeShared<FMCPTextureCommands>();
    MeshImportCommands = MakeShared<FMCPMeshImportCommands>();
    SoundImportCommands = MakeShared<FMCPSoundImportCommands>();
    FontImportCommands = MakeShared<FMCPFontImportCommands>();
    LevelCommands = MakeShared<FMCPLevelCommands>();
    DataAssetCommands = MakeShared<FMCPDataAssetCommands>();
    ReflectionCommands = MakeShared<FMCPReflectionCommands>();
    InspectionCommands = MakeShared<FMCPInspectionCommands>();
    SceneCommands = MakeShared<FMCPSceneCommands>();
    LandscapeCommands = MakeShared<FMCPLandscapeCommands>();
    FoliageCommands = MakeShared<FMCPFoliageCommands>();
    AssetFactoryCommands = MakeShared<FMCPAssetFactoryCommands>();
    WidgetCommands = MakeShared<FMCPWidgetCommands>();
    IKCommands = MakeShared<FMCPIKCommands>();
    GameplayTagCommands = MakeShared<FMCPGameplayTagCommands>();
    GASCommands = MakeShared<FMCPGASCommands>();
    DiagnosticsCommands = MakeShared<FMCPDiagnosticsCommands>();
    BlueprintPolishCommands = MakeShared<FMCPBlueprintPolishCommands>();
    KinematicsCommands = MakeShared<FMCPKinematicsCommands>();
    StreamingCommands = MakeShared<FMCPStreamingCommands>(this);
}

UMCPBridge::~UMCPBridge()
{
    EditorCommands.Reset();
    BlueprintCommands.Reset();
    BlueprintGraphCommands.Reset();
    MaterialCommands.Reset();
    AutomationCommands.Reset();
    PCGCommands.Reset();
    StateTreeCommands.Reset();
    EQSCommands.Reset();
    AIRuntimeCommands.Reset();
    SkeletalMeshCommands.Reset();
    MeshCommands.Reset();
    TextureCommands.Reset();
    MeshImportCommands.Reset();
    SoundImportCommands.Reset();
    FontImportCommands.Reset();
    DataAssetCommands.Reset();
    ReflectionCommands.Reset();
    InspectionCommands.Reset();
    SceneCommands.Reset();
    LandscapeCommands.Reset();
    FoliageCommands.Reset();
    AssetFactoryCommands.Reset();
    WidgetCommands.Reset();
    IKCommands.Reset();
    GameplayTagCommands.Reset();
    GASCommands.Reset();
    DiagnosticsCommands.Reset();
    BlueprintPolishCommands.Reset();
    KinematicsCommands.Reset();
    StreamingCommands.Reset();
}

// Initialize subsystem
void UMCPBridge::Initialize(FSubsystemCollectionBase& Collection)
{
    UE_LOG(LogUnrealMCP, Display, TEXT("MCPBridge: Initializing"));

    bIsRunning = false;
    ListenerSocket = nullptr;
    ConnectionSocket = nullptr;
    ServerThread = nullptr;
    Port = MCP_SERVER_PORT;
    FIPv4Address::Parse(MCP_SERVER_HOST, ServerAddress);

    // ── Boot gate ───────────────────────────────────────────────────────────
    // The command gate stays CLOSED until the editor is fully initialized. The
    // authoritative signal is FEditorDelegates::OnEditorInitialized (broadcast at
    // the tail of FUnrealEdMisc::OnInit). EditorSubsystem::Initialize runs earlier
    // in startup, so binding here reliably catches that later broadcast.
    //
    // Defensive already-booted check: if this subsystem is ever (re)created after
    // the engine main loop is already running (e.g. a plugin reload), the delegate
    // may never fire again — GIsRunning is true by then, so open the gate now to
    // avoid a permanent lockout. During normal early-boot Initialize GIsRunning is
    // false, so the gate correctly stays closed until the delegate fires.
    bEditorInteractive = (GIsRunning != 0);
    EditorInitializedHandle = FEditorDelegates::OnEditorInitialized.AddLambda(
        [this](double /*EditorStartupTime*/)
        {
            bEditorInteractive = true;
            UE_LOG(LogUnrealMCP, Display,
                TEXT("MCPBridge: Editor fully initialized — MCP command gate opened"));
            if (LogCollector)
            {
                LogCollector->InjectEvent(TEXT("MCP"), TEXT("Lifecycle"), ELogVerbosity::Display,
                    TEXT("Editor fully initialized — command gate opened (mcp_status.ready=true)"));
            }
        });

    // ── PIE gate ──────────────────────────────────────────────────────────────
    // Track whether a Play-In-Editor / Simulate-In-Editor session is running so
    // ExecuteCommand can refuse asset-load/mutation commands during PIE (they fail
    // with a misleading error because UEditorAssetLibrary::LoadAsset returns null
    // while GEditor->PlayWorld is set). Written here on the game thread via the
    // BeginPIE/EndPIE delegates; read on the server thread. Defensive seed for a
    // mid-session subsystem (re)create: GEditor is valid by then so query it.
    bPlayInEditorActive = (GEditor && GEditor->IsPlaySessionInProgress());
    BeginPIEHandle = FEditorDelegates::BeginPIE.AddLambda(
        [this](const bool /*bIsSimulating*/) { bPlayInEditorActive = true; });
    EndPIEHandle = FEditorDelegates::EndPIE.AddLambda(
        [this](const bool /*bIsSimulating*/) { bPlayInEditorActive = false; });

    // Initialize unified log collector (must be before StartServer so it
    // captures any log output from the server startup)
    LogCollector = MakeUnique<FMCPLogCollector>();
    LogCollector->Initialize();

    // Start the server automatically
    StartServer();
}

// Clean up resources when subsystem is destroyed
void UMCPBridge::Deinitialize()
{
    UE_LOG(LogUnrealMCP, Display, TEXT("MCPBridge: Shutting down"));

    if (EditorInitializedHandle.IsValid())
    {
        FEditorDelegates::OnEditorInitialized.Remove(EditorInitializedHandle);
        EditorInitializedHandle.Reset();
    }
    if (BeginPIEHandle.IsValid())
    {
        FEditorDelegates::BeginPIE.Remove(BeginPIEHandle);
        BeginPIEHandle.Reset();
    }
    if (EndPIEHandle.IsValid())
    {
        FEditorDelegates::EndPIE.Remove(EndPIEHandle);
        EndPIEHandle.Reset();
    }

    StopServer();

    // Shut down log collector after server so it captures server-stop log lines
    if (LogCollector)
    {
        LogCollector->Shutdown();
        LogCollector.Reset();
    }
}

// Start the MCP server
void UMCPBridge::StartServer()
{
    if (bIsRunning)
    {
        UE_LOG(LogUnrealMCP, Warning, TEXT("MCPBridge: Server is already running"));
        return;
    }

    // Create socket subsystem
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("MCPBridge: Failed to get socket subsystem"));
        return;
    }

    // Create listener socket
    TSharedPtr<FSocket> NewListenerSocket = MakeShareable(SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UnrealMCPListener"), false));
    if (!NewListenerSocket.IsValid())
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("MCPBridge: Failed to create listener socket"));
        return;
    }

    // Allow address reuse for quick restarts
    NewListenerSocket->SetReuseAddr(true);
    NewListenerSocket->SetNonBlocking(true);

    // Bind to address
    FIPv4Endpoint Endpoint(ServerAddress, Port);
    if (!NewListenerSocket->Bind(*Endpoint.ToInternetAddr()))
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("MCPBridge: Failed to bind listener socket to %s:%d"), *ServerAddress.ToString(), Port);
        return;
    }

    // Start listening
    if (!NewListenerSocket->Listen(5))
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("MCPBridge: Failed to start listening"));
        return;
    }

    ListenerSocket = NewListenerSocket;
    bIsRunning = true;
    UE_LOG(LogUnrealMCP, Display, TEXT("MCPBridge: Server started on %s:%d"), *ServerAddress.ToString(), Port);

    // Start server thread
    ServerThread = FRunnableThread::Create(
        new FMCPServerRunnable(this, ListenerSocket),
        TEXT("UnrealMCPServerThread"),
        0, TPri_Normal
    );

    if (!ServerThread)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("MCPBridge: Failed to create server thread"));
        StopServer();
        return;
    }
}

// Stop the MCP server
void UMCPBridge::StopServer()
{
    if (!bIsRunning)
    {
        return;
    }

    bIsRunning = false;

    // Clean up thread
    if (ServerThread)
    {
        ServerThread->Kill(true);
        delete ServerThread;
        ServerThread = nullptr;
    }

    // Close sockets
    if (ConnectionSocket.IsValid())
    {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ConnectionSocket.Get());
        ConnectionSocket.Reset();
    }

    if (ListenerSocket.IsValid())
    {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenerSocket.Get());
        ListenerSocket.Reset();
    }

    UE_LOG(LogUnrealMCP, Display, TEXT("MCPBridge: Server stopped"));
}

// Execute a command received from a client
FString UMCPBridge::ExecuteCommand(const FString& InCommandType, const TSharedPtr<FJsonObject>& Params)
{
    // Domain-first naming alias (todo/14_naming_migration.md): callers may pass
    // canonical names (`asset_rename`, `bp_compile`, etc.); the resolver maps them
    // to the legacy dispatched names. Net-add — unrecognized inputs pass through.
    // The resolved name shadows InCommandType so the rest of the dispatch chain
    // matches against the canonical-resolved string without per-site edits.
    const FString CommandType = InCommandType;  // canonical names dispatch directly (AliasMap removed)

    if (CommandType != InCommandType)
    {
        UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPBridge: Executing command: %s (resolved from canonical: %s)"), *CommandType, *InCommandType);
    }
    else
    {
        UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPBridge: Executing command: %s"), *CommandType);
    }

    // Inject MCP command event into unified log
    if (LogCollector)
    {
        LogCollector->InjectEvent(TEXT("MCP"), TEXT("Command"), ELogVerbosity::Display,
            FString::Printf(TEXT("Received: %s"), *CommandType));
    }

    // -----------------------------------------------------------------------
    // Boot gate — runs on the SERVER thread, before ANY game-thread dispatch.
    //
    // The TCP listener binds during this subsystem's Initialize() (mid
    // UEditorEngine::Init), so commands can arrive long before the editor is
    // interactive. Queuing their AsyncTask onto the game thread during that
    // window races engine init and reliably crashes startup — the bug family
    // documented in docs/bugs/mcp.md ("Sidecar replays queued/in-flight
    // commands too eagerly", "EditorPerformance tab spawn", "Queued OpenAsset").
    //
    // The fix the bug log prescribes: gate ALL dispatch on one boot-complete
    // signal, refusing on THIS thread so no game-thread work is ever queued
    // mid-init. (Per-handler !GIsRunning guards are too late — they run inside
    // the already-queued game-thread lambda, after the damage is done.)
    //
    //   * mcp_status — always answered here, synchronously, never touches the
    //     game thread. The Python sidecar polls it to decide when to dispatch.
    //   * ping       — allowed through (handled below) as a liveness probe.
    //   * everything else — refused with editor_not_ready until the gate opens.
    // -----------------------------------------------------------------------
    if (CommandType == TEXT("mcp_status"))
    {
        const bool bReady = bEditorInteractive;
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("ready"), bReady);
        Result->SetStringField(TEXT("phase"), bReady ? TEXT("interactive") : TEXT("initializing"));
        // Surface PIE state so the sidecar/agent can see it without provoking a
        // refusal — asset-authoring commands are gated while this is true.
        Result->SetBoolField(TEXT("pie_active"), (bool)bPlayInEditorActive);
        // GAP-060: surface an in-flight Live Coding compile so a caller can tell
        // "compiling" (game thread busy, bridge still answering here) from "crashed"
        // (no answer at all). Answered purely on this server thread — never touches
        // the game thread — so it stays available while the compile blocks it.
        {
            FScopeLock Lock(&LiveCodingCS);
            Result->SetBoolField(TEXT("live_coding_in_progress"),
                LiveCodingPhase == ELiveCodingPhase::Compiling);
        }
        // Pixel Streaming state (portable.dev#19 M2) — read from the bridge's
        // cache, NEVER from the PS2 modules: this reply is produced on the
        // network thread, and the PS2 accessors are game-thread shaped. The
        // cache is written by the game-thread stream_* handlers + the editor
        // streamer's start/stop delegates (see FMCPStreamingCommands).
        {
            const FMCPStreamState Stream = GetStreamState();
            TSharedRef<FJsonObject> StreamObj = MakeShared<FJsonObject>();
            StreamObj->SetBoolField(TEXT("active"), Stream.bActive);
            if (Stream.bViewerPortKnown)
            {
                StreamObj->SetNumberField(TEXT("viewer_port"), Stream.ViewerPort);
            }
            else
            {
                StreamObj->SetField(TEXT("viewer_port"), MakeShared<FJsonValueNull>());
            }
            TArray<TSharedPtr<FJsonValue>> StreamerValues;
            for (const FString& StreamerId : Stream.Streamers)
            {
                StreamerValues.Add(MakeShared<FJsonValueString>(StreamerId));
            }
            StreamObj->SetArrayField(TEXT("streamers"), StreamerValues);
            Result->SetObjectField(TEXT("stream"), StreamObj);
        }

        TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
        Envelope->SetStringField(TEXT("status"), TEXT("success"));
        Envelope->SetObjectField(TEXT("result"), Result);
        return MCPSerializeJson(Envelope);
    }

    if (!bEditorInteractive && CommandType != TEXT("ping"))
    {
        TSharedPtr<FJsonObject> InnerErr = FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(
                TEXT("Editor is still initializing; command '%s' was not dispatched. ")
                TEXT("Wait for readiness before retrying."), *CommandType),
            EMCPErrorCode::EditorNotReady,
            TEXT("The command gate opens when the editor finishes initializing. ")
            TEXT("Poll mcp_status until result.ready == true, then retry."));

        UE_LOG(LogUnrealMCP, Verbose,
            TEXT("MCPBridge: gate closed — refused '%s' (editor not yet interactive)"),
            *CommandType);
        return MCPWrapErrorEnvelope(InnerErr);
    }

    // -----------------------------------------------------------------------
    // PIE gate — runs on the SERVER thread, same as the boot gate above and for
    // the same reason: refuse BEFORE any game-thread dispatch. While a
    // Play-In-Editor session is active, the editor's asset-load path
    // (UEditorAssetLibrary::LoadAsset → CheckIfInEditorAndPIE) returns null, so
    // every asset-mutating/loading command fails — historically with a confusing
    // "asset not found" / intermittent socket error that cost hours of
    // misdiagnosis (docs/bugs/mcp.md). Refuse the asset command set explicitly
    // with a clear, recoverable pie_active error. PIE-driving automation, AI
    // reads, and registry/world/status reads are NOT in the blocklist, so they
    // continue to work during PIE (the whole point of running PIE).
    // -----------------------------------------------------------------------
    if (bPlayInEditorActive && FMCPCommonUtils::IsBlockedDuringPie(CommandType))
    {
        TSharedPtr<FJsonObject> InnerErr = FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(
                TEXT("PIE IS RUNNING — command '%s' was not dispatched. Asset load/mutation ")
                TEXT("commands cannot run while a Play-In-Editor session is active."), *CommandType),
            EMCPErrorCode::PieActive,
            TEXT("Ask the user to stop PIE (or call stop_pie), then retry. During PIE the ")
            TEXT("editor refuses asset loads, so this command would otherwise fail with a ")
            TEXT("misleading 'asset not found' / socket error. Read-only registry queries, ")
            TEXT("PIE input/screenshots, and AI-runtime reads remain available during PIE."));

        UE_LOG(LogUnrealMCP, Verbose,
            TEXT("MCPBridge: PIE gate — refused '%s' (Play-In-Editor active)"),
            *CommandType);
        return MCPWrapErrorEnvelope(InnerErr);
    }

    // -----------------------------------------------------------------------
    // Live Coding — non-blocking ticket + poll (GAP-060).
    //
    // A Live Coding compile (AutomationCommands_LiveCodingStart →
    // ILiveCodingModule::Compile with WaitForCompletion) blocks the GAME thread for
    // its whole duration to capture the honest Success/NoChanges/Failure result.
    // It must NOT also block THIS server thread: the single server thread both
    // accepts connections and runs each command inline (FMCPServerRunnable::Run),
    // and the client is connection-per-command, so a blocking wait here starves
    // EVERY other command — even mcp_status, which needs no game thread — to a
    // socket timeout, making the editor look crashed for the whole compile.
    //
    // Instead: kick the compile onto the game thread, route its eventual result to
    // FinishLiveCoding (via a TFuture continuation that fires on the game thread
    // when the compile resolves the promise), and return a `live_coding_in_progress`
    // ticket IMMEDIATELY. The server thread then loops back to accept/answer other
    // connections; mcp_status keeps reporting live_coding_in_progress=true so a
    // caller can distinguish "compiling" from "dead". The client polls mcp_status,
    // then re-issues this command with poll:true to collect the stored result.
    //
    // All Phase/Result access is under LiveCodingCS — shared between this server
    // thread and the game-thread completion continuation.
    // -----------------------------------------------------------------------
    if (CommandType == TEXT("editor_live_coding_compile"))
    {
        bool bPoll = false;
        if (Params.IsValid())
        {
            Params->TryGetBoolField(TEXT("poll"), bPoll);
        }

        FScopeLock Lock(&LiveCodingCS);

        // Retrieval path: collect the finished result once the compile has cleared.
        if (bPoll)
        {
            if (LiveCodingPhase == ELiveCodingPhase::Done)
            {
                FString Done = LiveCodingResult;
                LiveCodingResult.Reset();
                LiveCodingPhase = ELiveCodingPhase::Idle;
                return Done;
            }
            if (LiveCodingPhase == ELiveCodingPhase::Compiling)
            {
                return MCPLiveCodingTicket(); // still running — keep polling
            }
            // Idle with nothing pending: already collected, or no compile ran.
            TSharedRef<FJsonObject> IdleResult = MakeShared<FJsonObject>();
            IdleResult->SetBoolField(TEXT("live_coding_in_progress"), false);
            IdleResult->SetStringField(TEXT("phase"), TEXT("idle"));
            IdleResult->SetStringField(TEXT("message"),
                TEXT("No Live Coding compile result is pending (already collected, or none started)."));
            TSharedRef<FJsonObject> IdleEnv = MakeShared<FJsonObject>();
            IdleEnv->SetStringField(TEXT("status"), TEXT("success"));
            IdleEnv->SetObjectField(TEXT("result"), IdleResult);
            return MCPSerializeJson(IdleEnv);
        }

        // Start path. If a compile is already running, don't start another —
        // hand back the same ticket so the caller polls the in-flight one.
        if (LiveCodingPhase == ELiveCodingPhase::Compiling)
        {
            return MCPLiveCodingTicket();
        }

        // Kick a fresh compile on the game thread. The promise is resolved inside
        // AutomationCommands_LiveCodingStart (on the game thread); the continuation
        // below therefore runs on the game thread and stores the result. The promise
        // is brand-new and unfulfilled, so .Next() cannot fire synchronously here —
        // no re-entrancy into LiveCodingCS while we hold it.
        LiveCodingPhase = ELiveCodingPhase::Compiling;
        LiveCodingResult.Reset();

        TSharedPtr<TPromise<FString>> CompilePromise = MakeShared<TPromise<FString>>();
        TWeakObjectPtr<UMCPBridge> WeakThis(this);
        CompilePromise->GetFuture().Next([WeakThis](FString CompileResult)
        {
            if (UMCPBridge* Bridge = WeakThis.Get())
            {
                Bridge->FinishLiveCoding(CompileResult);
            }
        });

        FMCPLogCollector* Collector = LogCollector.Get();
        AsyncTask(ENamedThreads::GameThread, [CompilePromise, Collector]()
        {
            AutomationCommands_LiveCodingStart(CompilePromise, Collector);
        });

        return MCPLiveCodingTicket();
    }

    // Create a promise to wait for the result. Heap-allocated + shared so the game-thread handler body
    // stays a COPYABLE functor — a move-only TPromise capture can't bind through FTickerDelegate
    // (CreateLambda stores the functor by value). Same pattern the live-coding path above uses.
    TSharedPtr<TPromise<FString>> Promise = MakeShared<TPromise<FString>>();
    TFuture<FString> Future = Promise->GetFuture();

    // Build the game-thread handler body, then dispatch it below from a top-level tick (see GAP-057).
    auto GameThreadBody = [this, CommandType, Params, Promise]() mutable
    {
        // MCP commands are non-interactive. Flip GIsRunningUnattendedScript so
        // editor code that checks `FApp::IsUnattended() || GIsRunningUnattendedScript`
        // (UAssetToolsImpl::CanShowDialogs, SFileListReportDialog::OpenListDialog,
        // SCC checkout prompts, "Save before close?" prompts, the
        // "Redirector Update Report" modal, etc.) skips the dialog and takes
        // the safe default. Without this, any tool that triggers a confirmation
        // modal parks the game thread inside Slate's modal loop and the bridge
        // socket times out — see docs/bugs/mcp.md "Modal dialog wedges bridge".
        // Engine precedent: AssetTools.cpp:3500 wraps automated imports the same way.
        TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);

        TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject);

        try
        {
            TSharedPtr<FJsonObject> ResultJson;

            // doc-13 safety net: when the caller passes dry_run=true to a known
            // mutator that does NOT support dry_run yet (currently `add_node`),
            // short-circuit with the doc-13 mid-rollout contract response
            // (success=false, error_code=dry_run_unsupported) so the agent gets
            // a clear signal rather than silent dispatch-with-dry_run-ignored.
            // Net-add: read tools and dry_run-supporting mutators pass through.
            if (FMCPCommonUtils::ParseDryRun(Params) &&
                FMCPCommonUtils::IsBlockedFromDryRun(CommandType))
            {
                ResultJson = FMCPCommonUtils::CreateDryRunUnsupportedResponse(CommandType);
            }
            else if (CommandType == TEXT("ping"))
            {
                ResultJson = MakeShareable(new FJsonObject);
                ResultJson->SetStringField(TEXT("message"), TEXT("pong"));
            }
            // Editor Commands (including actor manipulation)
            else if (CommandType == TEXT("actor_get_in_level") ||
                     CommandType == TEXT("find_actors_by_name") ||
                     CommandType == TEXT("spawn_actor") ||
                     CommandType == TEXT("actor_delete") ||
                     CommandType == TEXT("actor_set_transform") ||
                     CommandType == TEXT("actor_set_property") ||
                     CommandType == TEXT("spawn_blueprint_actor") ||
                     CommandType == TEXT("asset_datatable_read") ||
                     CommandType == TEXT("level_set_gamemode_override"))
            {
                ResultJson = EditorCommands->HandleCommand(CommandType, Params);
            }
            // Blueprint Commands
            else if (CommandType == TEXT("bp_create_blueprint") ||
                     CommandType == TEXT("bp_set_default_value") ||
                     CommandType == TEXT("bp_add_component") ||
                     CommandType == TEXT("bp_set_component_transform") ||
                     CommandType == TEXT("bp_set_component_property") ||
                     CommandType == TEXT("bp_set_class_replication") ||
                     CommandType == TEXT("physics_set_properties") ||
                     CommandType == TEXT("bp_compile") ||
                     CommandType == TEXT("mesh_set_static_mesh_properties") ||
                     CommandType == TEXT("mesh_set_mesh_material_color") ||
                     CommandType == TEXT("material_get_available") ||
                     CommandType == TEXT("material_apply_to_actor") ||
                     CommandType == TEXT("material_apply_to_blueprint") ||
                     CommandType == TEXT("mesh_set_static_mesh_material") ||
                     CommandType == TEXT("mesh_get_actor_material_info") ||
                     CommandType == TEXT("get_blueprint_material_info") ||
                     CommandType == TEXT("bp_read") ||
                     CommandType == TEXT("bp_inspect") ||
                     CommandType == TEXT("bp_get_variable_details") ||
                     CommandType == TEXT("bp_get_function_details") ||
                     CommandType == TEXT("reflection_class_properties") ||
                     CommandType == TEXT("get_mesh_bounds") ||
                     CommandType == TEXT("bp_list_graphs") ||
                     CommandType == TEXT("bp_reparent") ||
                     CommandType == TEXT("anim_list_skeletons") ||
                     CommandType == TEXT("anim_list_sequences") ||
                     CommandType == TEXT("anim_blend_space_create") ||
                     CommandType == TEXT("anim_blend_space_add_sample") ||
                     CommandType == TEXT("anim_blend_space_remove_sample") ||
                     CommandType == TEXT("anim_sequence_set_property") ||
                     CommandType == TEXT("anim_skeleton_list_sockets") ||
                     CommandType == TEXT("anim_skeleton_add_socket") ||
                     CommandType == TEXT("anim_skeleton_modify_socket") ||
                     CommandType == TEXT("anim_skeleton_remove_socket") ||
                     CommandType == TEXT("bp_list_node_pins") ||
                     CommandType == TEXT("anim_list_blend_spaces") ||
                     CommandType == TEXT("anim_blend_space_read") ||
                     CommandType == TEXT("anim_list_blueprints") ||
                     CommandType == TEXT("anim_list_montages") ||
                     CommandType == TEXT("anim_list_layer_interfaces") ||
                     CommandType == TEXT("anim_montage_read"))
            {
                ResultJson = BlueprintCommands->HandleCommand(CommandType, Params);
            }
            // Blueprint Graph Commands
            else if (CommandType == TEXT("add_blueprint_node") ||
                     CommandType == TEXT("bp_connect_pins") ||
                     CommandType == TEXT("bp_create_variable") ||
                     CommandType == TEXT("bp_delete_variable") ||
                     CommandType == TEXT("bp_set_variable_properties") ||
                     CommandType == TEXT("bp_add_event_node") ||
                     CommandType == TEXT("bp_add_custom_event") ||
                     CommandType == TEXT("bp_delete_node") ||
                     CommandType == TEXT("bp_set_node_property") ||
                     CommandType == TEXT("bp_create_function") ||
                     CommandType == TEXT("bp_add_function_input") ||
                     CommandType == TEXT("bp_add_function_output") ||
                     CommandType == TEXT("bp_remove_function_input") ||
                     CommandType == TEXT("bp_remove_function_output") ||
                     CommandType == TEXT("bp_delete_function") ||
                     CommandType == TEXT("bp_rename_function") ||
                     CommandType == TEXT("bp_create_dispatcher"))
            {
                ResultJson = BlueprintGraphCommands->HandleCommand(CommandType, Params);
            }
            // Material Graph Commands
            else if (CommandType == TEXT("material_create") ||
                     CommandType == TEXT("material_set_property") ||
                     CommandType == TEXT("material_create_instance") ||
                     CommandType == TEXT("material_read") ||
                     CommandType == TEXT("material_read_function") ||
                     CommandType == TEXT("material_read_instance") ||
                     CommandType == TEXT("material_instance_set_parameter") ||
                     CommandType == TEXT("material_reparent_instance") ||
                     CommandType == TEXT("material_compile") ||
                     CommandType == TEXT("material_add_expression") ||
                     CommandType == TEXT("material_set_expression_property") ||
                     CommandType == TEXT("material_delete_expression") ||
                     CommandType == TEXT("material_connect") ||
                     CommandType == TEXT("list_material_parameters"))
            {
                ResultJson = MaterialCommands->HandleCommand(CommandType, Params);
            }
            // Texture Commands
            else if (CommandType == TEXT("asset_textures_import"))
            {
                ResultJson = TextureCommands->HandleCommand(CommandType, Params);
            }
            // Mesh / FBX Import Commands (GAP-001: external mesh import)
            else if (CommandType == TEXT("asset_import_mesh"))
            {
                ResultJson = MeshImportCommands->HandleCommand(CommandType, Params);
            }
            // Audio Import Commands (external .wav/.mp3/.ogg/… → USoundWave)
            else if (CommandType == TEXT("asset_import_audio"))
            {
                ResultJson = SoundImportCommands->HandleCommand(CommandType, Params);
            }
            // Font Import Commands (external .ttf/.otf → UFontFace + runtime UFont)
            else if (CommandType == TEXT("asset_import_font"))
            {
                ResultJson = FontImportCommands->HandleCommand(CommandType, Params);
            }
            // Level persistence Commands (GAP-006: new/save/save_as/load .umap) +
            // reflection-capture bake (fixes editor-vs-PIE reflection divergence).
            else if (CommandType == TEXT("level_new") ||
                     CommandType == TEXT("level_save") ||
                     CommandType == TEXT("level_save_as") ||
                     CommandType == TEXT("level_load") ||
                     CommandType == TEXT("editor_build_reflection_captures"))
            {
                ResultJson = LevelCommands->HandleCommand(CommandType, Params);
            }
            // Data Asset Commands — author/edit/inspect any UDataAsset subclass
            else if (CommandType == TEXT("asset_dataasset_create") ||
                     CommandType == TEXT("asset_dataasset_set_property") ||
                     CommandType == TEXT("asset_dataasset_read"))
            {
                ResultJson = DataAssetCommands->HandleCommand(CommandType, Params);
            }
            // Reflection Commands — read-only UClass/UEnum/UFunction inspection (todo/2_reflection_expansion.md)
            else if (CommandType == TEXT("enum_inspect") ||
                     CommandType == TEXT("class_query") ||
                     CommandType == TEXT("bp_function_references") ||
                     CommandType == TEXT("class_inspect"))
            {
                ResultJson = ReflectionCommands->HandleCommand(CommandType, Params);
            }
            // Inspection Commands — orient-rung briefs (todo/3_multi_res_inspection.md)
            else if (CommandType == TEXT("project_context") ||
                     CommandType == TEXT("scene_brief") ||
                     CommandType == TEXT("level_inspect") ||
                     CommandType == TEXT("bp_brief") ||
                     CommandType == TEXT("actor_inspect"))
            {
                ResultJson = InspectionCommands->HandleCommand(CommandType, Params);
            }
            // Scene Commands — actor query / spawn (todo/5_scene_query_compose.md)
            else if (CommandType == TEXT("actor_query") ||
                     CommandType == TEXT("actor_spawn"))
            {
                ResultJson = SceneCommands->HandleCommand(CommandType, Params);
            }
            // Landscape Commands — read-only inspection (GAPS #13)
            else if (CommandType == TEXT("landscape_inspect") ||
                     CommandType == TEXT("landscape_list_layers") ||
                     CommandType == TEXT("landscape_read_heightmap"))
            {
                ResultJson = LandscapeCommands->HandleCommand(CommandType, Params);
            }
            // Foliage Commands — read-only inspection (GAPS #14)
            else if (CommandType == TEXT("foliage_inspect"))
            {
                ResultJson = FoliageCommands->HandleCommand(CommandType, Params);
            }
            // Asset Factory Commands — primitive shells (todo/6_asset_factory.md)
            else if (CommandType == TEXT("enum_create") ||
                     CommandType == TEXT("struct_create") ||
                     CommandType == TEXT("datatable_create") ||
                     CommandType == TEXT("mpc_create") ||
                     CommandType == TEXT("material_function_create") ||
                     CommandType == TEXT("niagara_script_create") ||
                     CommandType == TEXT("input_create") ||
                     CommandType == TEXT("input_add_mapping") ||
                     CommandType == TEXT("physics_material_create"))
            {
                ResultJson = AssetFactoryCommands->HandleCommand(CommandType, Params);
            }
            // Widget Commands — UMG / UUserWidget authoring (todo/7_widget_umg.md)
            else if (CommandType == TEXT("widget_create") ||
                     CommandType == TEXT("widget_tree_read") ||
                     CommandType == TEXT("widget_add_child") ||
                     CommandType == TEXT("widget_bind_handler") ||
                     CommandType == TEXT("widget_set_property"))
            {
                ResultJson = WidgetCommands->HandleCommand(CommandType, Params);
            }
            // Animation retargeting — IK Rig + Retargeter authoring + batch exec.
            // Authoring mutators (create/set/auto-map) follow the universal
            // PreEditChange / mutate / PostEditChange / MarkPackageDirty contract.
            else if (CommandType == TEXT("ik_rig_list_chains") ||
                     CommandType == TEXT("ik_retarget_read") ||
                     CommandType == TEXT("ik_retarget_create") ||
                     CommandType == TEXT("ik_retarget_set_rigs") ||
                     CommandType == TEXT("ik_retarget_auto_map_chains") ||
                     CommandType == TEXT("ik_retarget_set_chain_mapping") ||
                     CommandType == TEXT("ik_retarget_import_pose_from_pose_asset") ||
                     CommandType == TEXT("ik_retarget_import_pose_from_animation") ||
                     CommandType == TEXT("ik_retarget_align_bones") ||
                     CommandType == TEXT("ik_retarget_run_batch") ||
                     CommandType == TEXT("ik_retarget_set_pelvis_settings") ||
                     CommandType == TEXT("ik_retarget_set_root_motion_settings") ||
                     CommandType == TEXT("anim_smooth_sequence") ||
                     CommandType == TEXT("anim_normalize_z_offset") ||
                     CommandType == TEXT("anim_anchor_feet_to_floor"))
            {
                ResultJson = IKCommands->HandleCommand(CommandType, Params);
            }
            // Gameplay Tag Registry — INI-backed CRUD (todo/9_gameplay_tag_registry.md)
            else if (CommandType == TEXT("tag_add") ||
                     CommandType == TEXT("tag_remove") ||
                     CommandType == TEXT("tag_list") ||
                     CommandType == TEXT("tag_move"))
            {
                ResultJson = GameplayTagCommands->HandleCommand(CommandType, Params);
            }
            // GAS Authoring — UGameplayAbility / UGameplayEffect Blueprint shells (todo/10_gas_authoring.md)
            else if (CommandType == TEXT("gas_ability_create") ||
                     CommandType == TEXT("gas_ability_set_cost") ||
                     CommandType == TEXT("gas_ability_set_cooldown") ||
                     CommandType == TEXT("gas_effect_create") ||
                     CommandType == TEXT("gas_effect_apply") ||
                     CommandType == TEXT("gas_attributeset_create"))
            {
                ResultJson = GASCommands->HandleCommand(CommandType, Params);
            }
            // Editor Diagnostics — perf snapshot + content-browser refresh (todo/11_editor_diagnostics.md)
            else if (CommandType == TEXT("editor_perf_snapshot") ||
                     CommandType == TEXT("editor_content_browser_refresh") ||
                     CommandType == TEXT("editor_window_screenshot"))
            {
                ResultJson = DiagnosticsCommands->HandleCommand(CommandType, Params);
            }
            // Blueprint Polish — small completeness fixes (todo/12_blueprint_polish.md)
            else if (CommandType == TEXT("bp_remove_component") ||
                     CommandType == TEXT("bp_disconnect_pin") ||
                     CommandType == TEXT("bp_get_parent_class") ||
                     CommandType == TEXT("bp_list_components"))
            {
                ResultJson = BlueprintPolishCommands->HandleCommand(CommandType, Params);
            }
            // Niagara Commands
            else if (CommandType == TEXT("niagara_list_systems") ||
                     CommandType == TEXT("niagara_system_read") ||
                     CommandType == TEXT("niagara_emitter_read") ||
                     CommandType == TEXT("niagara_user_parameter_add") ||
                     CommandType == TEXT("niagara_user_parameter_remove") ||
                     CommandType == TEXT("niagara_user_parameter_set") ||
                     CommandType == TEXT("niagara_emitter_set_enabled") ||
                     CommandType == TEXT("niagara_module_get_inputs") ||
                     CommandType == TEXT("niagara_module_set_input") ||
                     CommandType == TEXT("niagara_scratch_pad_module_add") ||
                     CommandType == TEXT("niagara_system_create") ||
                     CommandType == TEXT("niagara_emitter_add") ||
                     CommandType == TEXT("niagara_emitter_add_renderer") ||
                     CommandType == TEXT("niagara_renderer_set_material") ||
                     CommandType == TEXT("niagara_renderer_set_material_binding") ||
                     CommandType == TEXT("niagara_module_add") ||
                     CommandType == TEXT("niagara_emitter_set_local_space") ||
                     CommandType == TEXT("niagara_renderer_set_alignment") ||
                     CommandType == TEXT("niagara_mesh_renderer_set_mesh") ||
                     CommandType == TEXT("niagara_renderer_set_enabled"))
            {
                ResultJson = NiagaraCommands->HandleCommand(CommandType, Params);
            }
            // PCG Commands (procedural content generation: graphs, nodes, components)
            else if (CommandType == TEXT("pcg_list_graphs") ||
                     CommandType == TEXT("pcg_list_node_types") ||
                     CommandType == TEXT("pcg_graph_read") ||
                     CommandType == TEXT("pcg_graph_create") ||
                     CommandType == TEXT("pcg_node_add") ||
                     CommandType == TEXT("pcg_node_connect") ||
                     CommandType == TEXT("pcg_node_set_property") ||
                     CommandType == TEXT("pcg_component_add") ||
                     CommandType == TEXT("pcg_component_generate"))
            {
                ResultJson = PCGCommands->HandleCommand(CommandType, Params);
            }
            // Animation Commands (state machines, notifies, montages, ABP creation)
            else if (CommandType == TEXT("anim_state_machine_create") ||
                     CommandType == TEXT("anim_state_machine_state_add") ||
                     CommandType == TEXT("add_conduit") ||
                     CommandType == TEXT("anim_state_machine_transition_add") ||
                     CommandType == TEXT("anim_state_machine_set_entry") ||
                     CommandType == TEXT("anim_state_machine_modify_transition") ||
                     CommandType == TEXT("anim_state_machine_state_remove") ||
                     CommandType == TEXT("anim_state_machine_transition_remove") ||
                     CommandType == TEXT("bp_set_inner_node_property") ||
                     CommandType == TEXT("anim_node_bind_property") ||
                     CommandType == TEXT("anim_list_notifies") ||
                     CommandType == TEXT("anim_notify_add") ||
                     CommandType == TEXT("anim_notify_remove") ||
                     CommandType == TEXT("anim_extract_between_notifies") ||
                     CommandType == TEXT("anim_montage_create") ||
                     CommandType == TEXT("anim_montage_add_section") ||
                     CommandType == TEXT("anim_montage_set_section_link") ||
                     CommandType == TEXT("anim_montage_set_blend") ||
                     CommandType == TEXT("anim_blueprint_create") ||
                     CommandType == TEXT("anim_blueprint_set_skeleton"))
            {
                ResultJson = AnimationCommands->HandleCommand(CommandType, Params);
            }
            // Skeletal Mesh Commands (cloth, sections, physics asset inspection + mutation)
            else if (CommandType == TEXT("anim_skeletal_mesh_inspect") ||
                     CommandType == TEXT("anim_skeletal_mesh_set_section_disabled") ||
                     CommandType == TEXT("anim_physics_inspect") ||
                     CommandType == TEXT("physics_set_body_collision") ||
                     CommandType == TEXT("physics_set_constraint_motion") ||
                     CommandType == TEXT("mesh_set_physics_asset") ||
                     CommandType == TEXT("merge_bones_into_skeleton") ||
                     CommandType == TEXT("merge_bones_into_skeletal_mesh") ||
                     CommandType == TEXT("mesh_build_bend_chain"))
            {
                ResultJson = SkeletalMeshCommands->HandleCommand(CommandType, Params);
            }
            // Static Mesh Commands (bake, collision authoring, socket authoring, future static-mesh asset ops)
            else if (CommandType == TEXT("asset_bake_dynamic_to_static_mesh") ||
                     CommandType == TEXT("mesh_set_collision") ||
                     CommandType == TEXT("mesh_get_collision") ||
                     CommandType == TEXT("mesh_list_sockets") ||
                     CommandType == TEXT("mesh_get_bounds") ||
                     CommandType == TEXT("mesh_add_socket") ||
                     CommandType == TEXT("mesh_modify_socket") ||
                     CommandType == TEXT("mesh_remove_socket"))
            {
                ResultJson = MeshCommands->HandleCommand(CommandType, Params);
            }
            // Automation Commands (PIE, screenshots, input, console)
            else if (CommandType == TEXT("pie_start") ||
                     CommandType == TEXT("pie_stop") ||
                     CommandType == TEXT("pie_get_state") ||
                     CommandType == TEXT("pie_query") ||
                     CommandType == TEXT("editor_screenshot") ||
                     CommandType == TEXT("editor_focus_actor") ||
                     CommandType == TEXT("pie_send_keystrokes") ||
                     CommandType == TEXT("pie_send_mouse") ||
                     CommandType == TEXT("editor_console_exec") ||
                     CommandType == TEXT("editor_viewport_get_camera") ||
                     CommandType == TEXT("pie_capture_from_pose") ||
                     CommandType == TEXT("pie_inject_input_action"))
            {
                ResultJson = AutomationCommands->HandleCommand(CommandType, Params);
            }
            // PIE video recording (capture primitive — see docs: PIE video plan)
            else if (CommandType == TEXT("pie_record_start") ||
                     CommandType == TEXT("pie_record_stop") ||
                     CommandType == TEXT("pie_record_status") ||
                     CommandType == TEXT("pie_record_arm") ||
                     CommandType == TEXT("pie_record_disarm"))
            {
                ResultJson = RecorderCommands->HandleCommand(CommandType, Params);
            }
            // Asset Management Commands
            else if (CommandType == TEXT("asset_rename"))
            {
                ResultJson = FAssetManager::RenameAsset(Params);
            }
            else if (CommandType == TEXT("asset_move"))
            {
                ResultJson = FAssetManager::MoveAsset(Params);
            }
            else if (CommandType == TEXT("asset_duplicate"))
            {
                ResultJson = FAssetManager::DuplicateAsset(Params);
            }
            else if (CommandType == TEXT("asset_save"))
            {
                ResultJson = FAssetManager::SaveAsset(Params);
            }
            else if (CommandType == TEXT("asset_list"))
            {
                ResultJson = FAssetManager::ListAssets(Params);
            }
            else if (CommandType == TEXT("asset_fixup_redirectors"))
            {
                ResultJson = FAssetManager::FixupRedirectors(Params);
            }
            else if (CommandType == TEXT("asset_delete"))
            {
                ResultJson = FAssetManager::DeleteAsset(Params);
            }
            else if (CommandType == TEXT("asset_open"))
            {
                ResultJson = FAssetManager::OpenAsset(Params);
            }
            else if (CommandType == TEXT("asset_references"))
            {
                ResultJson = FAssetManager::AssetReferences(Params);
            }
            // StateTree Commands
            else if (CommandType == TEXT("statetree_create") ||
                     CommandType == TEXT("statetree_read") ||
                     CommandType == TEXT("statetree_compile") ||
                     CommandType == TEXT("statetree_save") ||
                     CommandType == TEXT("statetree_verify") ||
                     CommandType == TEXT("statetree_list_node_types") ||
                     CommandType == TEXT("statetree_list_schemas") ||
                     CommandType == TEXT("st_add_state") ||
                     CommandType == TEXT("st_remove_state") ||
                     CommandType == TEXT("st_rename_state") ||
                     CommandType == TEXT("st_move_state") ||
                     CommandType == TEXT("st_duplicate_state") ||
                     CommandType == TEXT("st_set_state_properties") ||
                     CommandType == TEXT("st_list_states") ||
                     CommandType == TEXT("st_add_node") ||
                     CommandType == TEXT("st_remove_node") ||
                     CommandType == TEXT("st_set_node_property") ||
                     CommandType == TEXT("st_get_node_properties") ||
                     CommandType == TEXT("st_add_transition") ||
                     CommandType == TEXT("st_remove_transition") ||
                     CommandType == TEXT("st_set_transition_properties") ||
                     CommandType == TEXT("st_add_property_binding") ||
                     CommandType == TEXT("st_remove_property_binding") ||
                     CommandType == TEXT("st_list_property_bindings") ||
                     CommandType == TEXT("st_list_bindable_properties"))
            {
                ResultJson = StateTreeCommands->HandleCommand(CommandType, Params);
            }
            // EQS Commands
            else if (CommandType == TEXT("eqs_create") ||
                     CommandType == TEXT("eqs_read") ||
                     CommandType == TEXT("eqs_option_add") ||
                     CommandType == TEXT("eqs_test_add") ||
                     CommandType == TEXT("eqs_option_remove") ||
                     CommandType == TEXT("eqs_test_remove") ||
                     CommandType == TEXT("eqs_set_property") ||
                     CommandType == TEXT("eqs_list_types"))
            {
                ResultJson = EQSCommands->HandleCommand(CommandType, Params);
            }
            // AI Runtime Commands (require PIE)
            else if (CommandType == TEXT("ai_get_state") ||
                     CommandType == TEXT("ai_get_awareness") ||
                     CommandType == TEXT("ai_get_perception"))
            {
                ResultJson = AIRuntimeCommands->HandleCommand(CommandType, Params);
            }
            // Kinematics Commands — editor-world component-space transform math
            // (read bone/socket transforms, forward FK probe, inverse two-bone solve).
            // See docs/mcp/POSITION_PROBE_TOOLS.md.
            else if (CommandType == TEXT("kinematics_read_transform") ||
                     CommandType == TEXT("kinematics_probe") ||
                     CommandType == TEXT("kinematics_solve"))
            {
                ResultJson = KinematicsCommands->HandleCommand(CommandType, Params);
            }
            // Pixel Streaming 2 control (portable.dev#19 M2). Deliberately NOT in
            // the PIE blocklist — streaming keeps running during PIE (AutoStreamPIE).
            else if (CommandType == TEXT("stream_start") ||
                     CommandType == TEXT("stream_stop") ||
                     CommandType == TEXT("stream_status"))
            {
                ResultJson = StreamingCommands->HandleCommand(CommandType, Params);
            }
            else
            {
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown command: %s"), *CommandType));
                
                FString ResultString;
                TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
                FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
                Promise->SetValue(ResultString);
                return;
            }
            
            // Check if the result contains an error
            bool bSuccess = true;
            FString ErrorMessage;
            FString ErrorCode;
            FString ErrorHint;

            if (ResultJson->HasField(TEXT("success")))
            {
                bSuccess = ResultJson->GetBoolField(TEXT("success"));
                if (!bSuccess)
                {
                    if (ResultJson->HasField(TEXT("error")))
                    {
                        ErrorMessage = ResultJson->GetStringField(TEXT("error"));
                    }
                    if (ResultJson->HasField(TEXT("error_code")))
                    {
                        ErrorCode = ResultJson->GetStringField(TEXT("error_code"));
                    }
                    if (ResultJson->HasField(TEXT("error_hint")))
                    {
                        ErrorHint = ResultJson->GetStringField(TEXT("error_hint"));
                    }
                }
            }

            // Inject structured compile events for known compile commands
            if (LogCollector)
            {
                if (CommandType == TEXT("bp_compile"))
                {
                    LogCollector->InjectEvent(TEXT("BLUEPRINT"), TEXT("Compile"),
                        bSuccess ? ELogVerbosity::Display : ELogVerbosity::Error,
                        bSuccess ? TEXT("Blueprint compiled successfully") : FString::Printf(TEXT("Blueprint compile failed: %s"), *ErrorMessage));
                }
                else if (CommandType == TEXT("material_compile"))
                {
                    LogCollector->InjectEvent(TEXT("MATERIAL"), TEXT("Compile"),
                        bSuccess ? ELogVerbosity::Display : ELogVerbosity::Error,
                        bSuccess ? TEXT("Material compiled successfully") : FString::Printf(TEXT("Material compile failed: %s"), *ErrorMessage));
                }
                else if (CommandType == TEXT("statetree_compile"))
                {
                    LogCollector->InjectEvent(TEXT("STATETREE"), TEXT("Compile"),
                        bSuccess ? ELogVerbosity::Display : ELogVerbosity::Error,
                        bSuccess ? TEXT("StateTree compiled successfully") : FString::Printf(TEXT("StateTree compile failed: %s"), *ErrorMessage));
                }
            }
            
            if (bSuccess)
            {
                // Set success status and include the result
                ResponseJson->SetStringField(TEXT("status"), TEXT("success"));
                ResponseJson->SetObjectField(TEXT("result"), ResultJson);
            }
            else
            {
                // Set error status and include the full result for structured errors
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetStringField(TEXT("error"), ErrorMessage);
                if (!ErrorCode.IsEmpty())
                {
                    ResponseJson->SetStringField(TEXT("error_code"), ErrorCode);
                }
                if (!ErrorHint.IsEmpty())
                {
                    ResponseJson->SetStringField(TEXT("error_hint"), ErrorHint);
                }
                ResponseJson->SetObjectField(TEXT("result"), ResultJson);
            }
        }
        catch (const std::exception& e)
        {
            ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
            ResponseJson->SetStringField(TEXT("error"), UTF8_TO_TCHAR(e.what()));
        }
        
        FString ResultString;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
        FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
        Promise->SetValue(ResultString);
    };

    // Dispatch EVERY command from a TOP-LEVEL game-thread tick (FTSTicker fires from FEngineLoop::Tick),
    // not via AsyncTask (which runs the body inside ProcessTasksNamedThread). At top level a handler may
    // freely flush / build / wait on the task graph — a mesh import building render data, an asset save,
    // a future heavy op — without re-entering named-thread task processing and tripping the RecursionGuard
    // assert (TaskGraph.cpp). This is the general fix: no command, present or future, runs nested.
    // FTSTicker::AddTicker is thread-safe to call from this server thread; the one-shot tick (return false)
    // runs the body once on the game thread, and the server thread keeps blocking on Future.Get() below
    // with its existing timeout. (The caller is synchronous either way, so the ≤1-frame defer is invisible.)
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [Body = MoveTemp(GameThreadBody)](float) mutable -> bool { Body(); return false; }), 0.f);

    FString FinalEnvelope = Future.Get();

    // GAP-007: editor_screenshot's editor-viewport path kicks an async
    // FScreenshotRequest serviced at end-of-frame; the game-thread handler returns a
    // "requested" envelope before any file exists (and must not block to wait — that
    // would starve the render). Confirm the file here on the SERVER thread (the game
    // thread stays free to render the invalidated frame), rewriting the envelope to an
    // honest success(path) or a `timeout` error instead of a false "requested".
    // pie_capture_from_pose also returns "requested" and writes its PNG from a
    // deferred ticker (after the view-target swap composites) — confirm it the same way.
    if (CommandType == TEXT("editor_screenshot") ||
        CommandType == TEXT("pie_capture_from_pose"))
    {
        FinalEnvelope = MCPAwaitScreenshotFile(FinalEnvelope);
    }

    return FinalEnvelope;
}

void UMCPBridge::SetStreamState(const FMCPStreamState& InState)
{
    FScopeLock Lock(&StreamStateCS);
    StreamState = InState;
}

FMCPStreamState UMCPBridge::GetStreamState() const
{
    FScopeLock Lock(&StreamStateCS);
    return StreamState;
}

void UMCPBridge::FinishLiveCoding(const FString& InResult)
{
    // Runs on the game thread (the compile resolves its promise there). Stores the
    // serialized result envelope and flips the phase to Done so a poll:true retrieval
    // can collect it. GAP-060.
    FScopeLock Lock(&LiveCodingCS);
    LiveCodingResult = InResult;
    LiveCodingPhase = ELiveCodingPhase::Done;
}