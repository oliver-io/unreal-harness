#include "Commands/MCPStreamingCommands.h"
#include "Commands/MCPCommonUtils.h"
#include "MCPBridge.h"

#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"

// Pixel Streaming 2 (hard plugin dependency — see UnrealMCP.uplugin/.Build.cs).
#include "IPixelStreaming2EditorModule.h"   // PixelStreaming2Editor: Start/StopStreaming, ports, signalling domain
#include "IPixelStreaming2Module.h"         // PixelStreaming2: GetStreamerIds / FindStreamer
#include "IPixelStreaming2Streamer.h"       // PixelStreaming2Core: IsStreaming, OnStreamingStarted/Stopped
#include "PixelStreaming2SettingsEnums.h"   // PixelStreaming2Settings: EPixelStreaming2EditorStreamTypes

namespace
{
    constexpr int32 MCPStreamDefaultViewerPort = 8890;
    constexpr int32 MCPStreamDefaultStreamerPort = 8888;

    // Module lookups. The Build.cs dependency guarantees the DLLs exist, but the
    // modules can still be unloaded (editor shutdown, -game, a host project that
    // force-disabled the plugin) — degrade to nullptr instead of the checked Get().
    IPixelStreaming2EditorModule* MCPGetPS2EditorModule(bool bLoadIfNeeded)
    {
        static const FName ModuleName(TEXT("PixelStreaming2Editor"));
        if (bLoadIfNeeded)
        {
            return FModuleManager::LoadModulePtr<IPixelStreaming2EditorModule>(ModuleName);
        }
        return FModuleManager::Get().IsModuleLoaded(ModuleName)
            ? FModuleManager::GetModulePtr<IPixelStreaming2EditorModule>(ModuleName)
            : nullptr;
    }

    IPixelStreaming2Module* MCPGetPS2Module(bool bLoadIfNeeded)
    {
        static const FName ModuleName(TEXT("PixelStreaming2"));
        if (bLoadIfNeeded)
        {
            return FModuleManager::LoadModulePtr<IPixelStreaming2Module>(ModuleName);
        }
        return FModuleManager::Get().IsModuleLoaded(ModuleName)
            ? FModuleManager::GetModulePtr<IPixelStreaming2Module>(ModuleName)
            : nullptr;
    }

    /** Live PS2 state (game thread). Returns false when PS2 is unavailable;
     *  out-params are then the inert zero-state. */
    bool MCPQueryStreamState(bool& bOutActive, int32& OutViewerPort, int32& OutStreamerPort, TArray<FString>& OutStreamers)
    {
        bOutActive = false;
        OutViewerPort = 0;
        OutStreamerPort = 0;
        OutStreamers.Reset();

        IPixelStreaming2EditorModule* EditorModule = MCPGetPS2EditorModule(/*bLoadIfNeeded=*/false);
        IPixelStreaming2Module* CoreModule = MCPGetPS2Module(/*bLoadIfNeeded=*/false);
        if (!EditorModule || !CoreModule)
        {
            return false;
        }

        for (const FString& StreamerId : CoreModule->GetStreamerIds())
        {
            const TSharedPtr<IPixelStreaming2Streamer> Streamer = CoreModule->FindStreamer(StreamerId);
            if (Streamer.IsValid() && Streamer->IsStreaming())
            {
                OutStreamers.Add(StreamerId);
            }
        }
        bOutActive = OutStreamers.Num() > 0;
        OutViewerPort = EditorModule->GetViewerPort();
        OutStreamerPort = EditorModule->GetStreamerPort();
        return true;
    }

    /** Set a PS2 console variable by name; missing CVars just warn (older/newer
     *  engine drops) instead of failing the command. */
    template <typename ValueType>
    void MCPSetPS2CVar(const TCHAR* Name, ValueType Value)
    {
        if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
        {
            CVar->Set(Value, ECVF_SetByCode);
        }
        else
        {
            UE_LOG(LogUnrealMCP, Warning, TEXT("MCPStreaming: console variable %s not found — skipped"), Name);
        }
    }

    /** Read an optional integer port param with a default; false + error response on garbage. */
    bool MCPReadPortParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32 Default,
        int32& OutPort, TSharedPtr<FJsonObject>& OutError)
    {
        OutPort = Default;
        if (Params.IsValid() && Params->HasField(Field))
        {
            double Raw = 0.0;
            if (!Params->TryGetNumberField(Field, Raw) || Raw != FMath::Floor(Raw))
            {
                OutError = FMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Parameter '%s' must be an integer port number"), Field),
                    EMCPErrorCode::InvalidArgument,
                    TEXT("Pass a TCP port in the range 1-65535, or omit the parameter for the default."));
                return false;
            }
            OutPort = static_cast<int32>(Raw);
        }
        if (OutPort < 1 || OutPort > 65535)
        {
            OutError = FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Parameter '%s' is out of range: %d"), Field, OutPort),
                EMCPErrorCode::OutOfRange,
                TEXT("Ports must be in the range 1-65535."));
            return false;
        }
        return true;
    }

    TSharedPtr<FJsonObject> MCPStreamingUnavailableError()
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Pixel Streaming 2 is unavailable — the PixelStreaming2 editor module is not loaded."),
            EMCPErrorCode::FeatureDisabled,
            TEXT("Enable the PixelStreaming2 plugin in the host project (.uproject) and restart the editor. ")
            TEXT("The UnrealMCP plugin declares the dependency, so a stock editor build should have it."));
    }
}

FMCPStreamingCommands::FMCPStreamingCommands(UMCPBridge* InBridge)
    : Bridge(InBridge)
{
}

TSharedPtr<FJsonObject> FMCPStreamingCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("stream_start"))
    {
        return HandleStreamStart(Params);
    }
    if (CommandType == TEXT("stream_stop"))
    {
        return HandleStreamStop(Params);
    }
    if (CommandType == TEXT("stream_status"))
    {
        return HandleStreamStatus(Params);
    }
    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown streaming command: %s"), *CommandType),
        EMCPErrorCode::InvalidArgument);
}

TSharedPtr<FJsonObject> FMCPStreamingCommands::HandleStreamStart(const TSharedPtr<FJsonObject>& Params)
{
    int32 ViewerPort = MCPStreamDefaultViewerPort;
    int32 StreamerPort = MCPStreamDefaultStreamerPort;
    TSharedPtr<FJsonObject> ParamError;
    if (!MCPReadPortParam(Params, TEXT("viewer_port"), MCPStreamDefaultViewerPort, ViewerPort, ParamError) ||
        !MCPReadPortParam(Params, TEXT("streamer_port"), MCPStreamDefaultStreamerPort, StreamerPort, ParamError))
    {
        return ParamError;
    }

    IPixelStreaming2EditorModule* EditorModule = MCPGetPS2EditorModule(/*bLoadIfNeeded=*/true);
    IPixelStreaming2Module* CoreModule = MCPGetPS2Module(/*bLoadIfNeeded=*/true);
    if (!EditorModule || !CoreModule)
    {
        return MCPStreamingUnavailableError();
    }

    // Idempotent: an already-live stream is success, reported at its CURRENT
    // ports (a second start with different ports does not rebind the running
    // signalling server — stop first to change ports).
    {
        bool bActive = false;
        int32 CurrentViewerPort = 0, CurrentStreamerPort = 0;
        TArray<FString> Streamers;
        if (MCPQueryStreamState(bActive, CurrentViewerPort, CurrentStreamerPort, Streamers) && bActive)
        {
            EnsureStreamerDelegatesBound();
            RefreshStreamStateCache();
            TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
            Result->SetBoolField(TEXT("success"), true);
            Result->SetStringField(TEXT("state"), TEXT("streaming"));
            Result->SetBoolField(TEXT("active"), true);
            Result->SetNumberField(TEXT("viewer_port"), CurrentViewerPort);
            Result->SetNumberField(TEXT("streamer_port"), CurrentStreamerPort);
            Result->SetStringField(TEXT("message"),
                TEXT("Pixel Streaming is already active — returning the current state. ")
                TEXT("Call stream_stop first to restart on different ports."));
            return Result;
        }
    }

    // Configure the editor-streaming CVars: stream the level viewport, follow
    // PIE sessions automatically, and use the EMBEDDED signalling server.
    MCPSetPS2CVar(TEXT("PixelStreaming2.Editor.Source"), TEXT("LevelEditorViewport"));
    MCPSetPS2CVar(TEXT("PixelStreaming2.Editor.AutoStreamPIE"), true);
    MCPSetPS2CVar(TEXT("PixelStreaming2.Editor.UseRemoteSignallingServer"), false);

    // Ports + domain BEFORE StartStreaming — the embedded signalling server is
    // launched with --HttpPort=<ViewerPort> --StreamerPort=<StreamerPort>, and
    // the Windows default ViewerPort is 80 (commonly occupied/privileged), so the
    // viewer port must always be set first.
    EditorModule->SetViewerPort(ViewerPort);
    EditorModule->SetStreamerPort(StreamerPort);
    EditorModule->SetSignallingDomain(TEXT("ws://127.0.0.1"));

    EditorModule->StartStreaming(EPixelStreaming2EditorStreamTypes::LevelEditorViewport);

    EnsureStreamerDelegatesBound();
    RefreshStreamStateCache();

    // Async by design: the first launch may still be DOWNLOADING the signalling
    // server bundle (needs internet, can take minutes), and the streamer's
    // signalling connection is established off-thread. Report "starting" and let
    // callers poll stream_status / mcp_status.stream for the live flag.
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("state"), TEXT("starting"));
    Result->SetBoolField(TEXT("active"), false);
    Result->SetNumberField(TEXT("viewer_port"), ViewerPort);
    Result->SetNumberField(TEXT("streamer_port"), StreamerPort);
    Result->SetStringField(TEXT("message"),
        TEXT("Editor-viewport Pixel Streaming is starting (embedded signalling server; the first ")
        TEXT("launch may download the server bundle). Poll stream_status until active is true, ")
        TEXT("then open http://127.0.0.1:<viewer_port> in a browser."));
    return Result;
}

TSharedPtr<FJsonObject> FMCPStreamingCommands::HandleStreamStop(const TSharedPtr<FJsonObject>& /*Params*/)
{
    bool bWasStreaming = false;

    // Idempotent by contract: no PS2, or PS2 with nothing running, is still a
    // successful stop — the desired end state ("not streaming") already holds.
    if (IPixelStreaming2EditorModule* EditorModule = MCPGetPS2EditorModule(/*bLoadIfNeeded=*/false))
    {
        int32 ViewerPort = 0, StreamerPort = 0;
        TArray<FString> Streamers;
        MCPQueryStreamState(bWasStreaming, ViewerPort, StreamerPort, Streamers);
        EditorModule->StopStreaming();
    }

    RefreshStreamStateCache();

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetBoolField(TEXT("stopped"), true);
    Result->SetBoolField(TEXT("was_streaming"), bWasStreaming);
    return Result;
}

TSharedPtr<FJsonObject> FMCPStreamingCommands::HandleStreamStatus(const TSharedPtr<FJsonObject>& /*Params*/)
{
    bool bActive = false;
    int32 ViewerPort = 0, StreamerPort = 0;
    TArray<FString> Streamers;
    const bool bAvailable = MCPQueryStreamState(bActive, ViewerPort, StreamerPort, Streamers);

    RefreshStreamStateCache();

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
    Result->SetBoolField(TEXT("success"), true);
    Result->SetBoolField(TEXT("active"), bActive);
    if (bAvailable)
    {
        Result->SetNumberField(TEXT("viewer_port"), ViewerPort);
        Result->SetNumberField(TEXT("streamer_port"), StreamerPort);
    }
    else
    {
        Result->SetField(TEXT("viewer_port"), MakeShared<FJsonValueNull>());
        Result->SetField(TEXT("streamer_port"), MakeShared<FJsonValueNull>());
    }
    TArray<TSharedPtr<FJsonValue>> StreamerValues;
    for (const FString& StreamerId : Streamers)
    {
        StreamerValues.Add(MakeShared<FJsonValueString>(StreamerId));
    }
    Result->SetArrayField(TEXT("streamers"), StreamerValues);
    return Result;
}

void FMCPStreamingCommands::RefreshStreamStateCache()
{
    if (!Bridge)
    {
        return;
    }

    FMCPStreamState State;
    int32 StreamerPort = 0;
    if (MCPQueryStreamState(State.bActive, State.ViewerPort, StreamerPort, State.Streamers))
    {
        State.bViewerPortKnown = true;
    }
    else
    {
        State = FMCPStreamState(); // inert zero-state when PS2 is unavailable
    }
    Bridge->SetStreamState(State);
}

void FMCPStreamingCommands::EnsureStreamerDelegatesBound()
{
    if (bStreamerDelegatesBound)
    {
        return;
    }

    IPixelStreaming2Module* CoreModule = MCPGetPS2Module(/*bLoadIfNeeded=*/false);
    if (!CoreModule)
    {
        return;
    }
    // The editor-viewport streamer registers with streamer ID "Editor"
    // (FPixelStreaming2EditorModule::InitEditorStreaming). An AutoStreamPIE-spawned
    // PIE streamer uses the default streamer ID and is NOT tracked here — the
    // stream_* handlers recompute the full state on every call, so the cache only
    // needs the cheap editor-streamer edge to catch out-of-band stops.
    const TSharedPtr<IPixelStreaming2Streamer> EditorStreamer = CoreModule->FindStreamer(TEXT("Editor"));
    if (!EditorStreamer.IsValid())
    {
        return;
    }

    // The delegates may fire off the game thread; SetStreamState is lock-guarded
    // and the lambdas re-guard bridge lifetime with a weak pointer, so this is
    // safe from any thread. They only patch the cached flags — no PS2 queries.
    TWeakObjectPtr<UMCPBridge> WeakBridge(Bridge);
    EditorStreamer->OnStreamingStarted().AddLambda([WeakBridge](IPixelStreaming2Streamer* Streamer)
    {
        if (UMCPBridge* PinnedBridge = WeakBridge.Get())
        {
            FMCPStreamState State = PinnedBridge->GetStreamState();
            State.bActive = true;
            State.Streamers.AddUnique(Streamer ? Streamer->GetId() : TEXT("Editor"));
            PinnedBridge->SetStreamState(State);
        }
    });
    EditorStreamer->OnStreamingStopped().AddLambda([WeakBridge](IPixelStreaming2Streamer* Streamer)
    {
        if (UMCPBridge* PinnedBridge = WeakBridge.Get())
        {
            FMCPStreamState State = PinnedBridge->GetStreamState();
            State.Streamers.Remove(Streamer ? Streamer->GetId() : TEXT("Editor"));
            State.bActive = State.Streamers.Num() > 0;
            PinnedBridge->SetStreamState(State);
        }
    });
    bStreamerDelegatesBound = true;
}
