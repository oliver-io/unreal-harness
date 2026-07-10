#include "Commands/MCPStreamingCommands.h"
#include "Commands/MCPCommonUtils.h"
#include "MCPBridge.h"

#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "Async/Async.h"                    // AsyncTask, ENamedThreads (defensive game-thread marshal)
#include "Serialization/MemoryReader.h"     // FMemoryReader (UIInteraction payload)
#include "Math/RotationMatrix.h"            // FRotationMatrix camera basis
#include "Misc/CommandLine.h"               // NVENC leak guard: command-line override parse
#include "Misc/ConfigCacheIni.h"            // NVENC leak guard: GGameIni D3D12UsesCUDA flag
#include "Misc/EngineVersion.h"             // NVENC leak guard: 5.7+ gate
#include "Misc/Parse.h"                     // NVENC leak guard: FParse::Bool
#include "RHI.h"                            // NVENC leak guard: RHIGetInterfaceType / IsRHIDeviceNVIDIA

// Editor level-viewport camera (touch-gesture camera control, portable.dev#19).
#include "Editor.h"                         // UnrealEd: GCurrentLevelEditingViewportClient, GEditor
#include "LevelEditorViewport.h"            // UnrealEd: FLevelEditorViewportClient
#include "EditorViewportClient.h"           // UnrealEd: Get/SetViewLocation/Rotation, Invalidate
#include "EngineUtils.h"                    // Engine: HActor hit proxy (tap-to-select / focus)

// PIE enter/exit from the phone ({t:"pie",on} — portable.dev#19 F3). Editor.h
// already pulls in EditorEngine.h → PlayInEditorDataTypes.h (FRequestPlaySessionParams).
#include "LevelEditor.h"                    // LevelEditor (private dep): FLevelEditorModule::GetFirstActive(Level)Viewport
#include "IAssetViewport.h"                 // UnrealEd: IAssetViewport → FRequestPlaySessionParams::DestinationSlateViewport

// Phone-aspect editor-window matching ({t:"viewres"} — portable.dev#19 feature 6).
// All modules already in UnrealMCP.Build.cs (Engine/UnrealEd public; LevelEditor/
// Slate/SlateCore private) — deliberately NO IMainFrameModule (module not a dep).
#include "SLevelViewport.h"                 // LevelEditor: SLevelViewport::GetLevelViewportClient
#include "StatusBarSubsystem.h"             // StatusBar: ForceDismissDrawer (PIE tap — the Output Log drawer shadows the viewport)
#include "SEditorViewport.h"                // UnrealEd: SEditorViewport::GetSceneViewport
#include "Slate/SceneViewport.h"            // Engine: FSceneViewport::FindWindow / GetSizeXY
#include "Widgets/SWindow.h"                // SlateCore: SWindow::Resize / GetClientSizeInScreen
#include "Framework/Application/SlateApplication.h" // Slate: FSlateApplication::GetWorkArea
#include "Containers/Ticker.h"              // Core: FTSTicker (deferred viewres + per-frame camera smoothing)
#include "HAL/PlatformTime.h"               // Core: FPlatformTime::Seconds (viewres debounce clock)
#include "Editor/EditorPerformanceSettings.h" // UnrealEd: disable foreground CPU throttle while streaming

// Pixel Streaming 2 (hard plugin dependency — see UnrealMCP.uplugin/.Build.cs).
#include "IPixelStreaming2EditorModule.h"   // PixelStreaming2Editor: Start/StopStreaming, ports, signalling domain
#include "IPixelStreaming2Module.h"         // PixelStreaming2: GetStreamerIds / FindStreamer
#include "IPixelStreaming2Streamer.h"       // PixelStreaming2Core: IsStreaming, OnStreamingStarted/Stopped, GetInputHandler
#include "IPixelStreaming2InputHandler.h"   // PixelStreaming2Input: RegisterMessageHandler (UIInteraction)
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

    // ── NVENC-D3D12 memory-leak guard (see ../mcp/docs/BUGS.md: "Editor OOM-killed
    // after long Pixel Streaming sessions") ──────────────────────────────────────
    // UE 5.7+ regression: FVideoEncoderNVENCD3D12::SendFrame registers/unregisters
    // the input texture and output bitstream with the driver EVERY encoded frame,
    // and recent NVIDIA drivers retain host memory across that churn (~65-115 MB/s
    // while any viewer is subscribed) until the editor is silently OOM-killed
    // (fail-fast inside nvEncodeAPI64.dll, no crash dialog). ue5-main added an
    // [AVCodecs.NvEnc] D3D12UsesCUDA flag that routes D3D12 frames through the
    // safe pre-5.7 CUDA pathway — but NO stock 5.7 engine has that code, so on a
    // standard engine the flag is inert and the default H.264 hardware path leaks.
    //
    // Guard: when this process would take the leaking path (5.7+, D3D12 RHI,
    // NVIDIA GPU, hardware codec) and D3D12UsesCUDA is NOT set, force the
    // verified-safe VP8 software codec before streaming starts. Users on a
    // patched source build or an engine that ships the flag opt back into NVENC
    // by setting [AVCodecs.NvEnc] D3D12UsesCUDA=1 in DefaultGame.ini (the exact
    // flag the engine itself reads), or -AVCodecs.NvEnc.D3D12UsesCUDA=true.
    void MCPApplyNvEncD3D12LeakGuard()
    {
#if PLATFORM_WINDOWS
        const FEngineVersion& Ver = FEngineVersion::Current();
        const bool bLeakProneEngine =
            Ver.GetMajor() > 5 || (Ver.GetMajor() == 5 && Ver.GetMinor() >= 7);
        if (!bLeakProneEngine || !GDynamicRHI)
        {
            return;
        }
        if (RHIGetInterfaceType() != ERHIInterfaceType::D3D12 || !IsRHIDeviceNVIDIA())
        {
            return;
        }

        // Mirror the engine-side read exactly (GGameIni, command line overrides).
        bool bD3D12UsesCUDA = false;
        GConfig->GetBool(TEXT("AVCodecs.NvEnc"), TEXT("D3D12UsesCUDA"), bD3D12UsesCUDA, GGameIni);
        FParse::Bool(FCommandLine::Get(), TEXT("AVCodecs.NvEnc.D3D12UsesCUDA="), bD3D12UsesCUDA);
        if (bD3D12UsesCUDA)
        {
            UE_LOG(LogUnrealMCP, Log,
                TEXT("MCPStreaming: NVENC leak guard — D3D12UsesCUDA is set; assuming an engine that ")
                TEXT("honors it (patched 5.7 source build or 5.8+) and keeping the hardware encoder. ")
                TEXT("On a STOCK 5.7 engine this flag is inert and the leak persists — if editor ")
                TEXT("private bytes climb while a viewer is connected, unset the flag to fall back to VP8."));
            return;
        }

        IConsoleVariable* CodecVar =
            IConsoleManager::Get().FindConsoleVariable(TEXT("PixelStreaming2.Encoder.Codec"));
        if (!CodecVar)
        {
            return; // no PS2 codec CVar (plugin unavailable) — nothing to guard
        }
        const FString Codec = CodecVar->GetString();
        if (Codec.Equals(TEXT("VP8"), ESearchCase::IgnoreCase) ||
            Codec.Equals(TEXT("VP9"), ESearchCase::IgnoreCase))
        {
            return; // already on a software codec — safe
        }

        CodecVar->Set(TEXT("VP8"), ECVF_SetByCode);
        UE_LOG(LogUnrealMCP, Warning,
            TEXT("MCPStreaming: NVENC leak guard — forced the Pixel Streaming codec from %s to VP8 ")
            TEXT("(software). This engine (%d.%d, D3D12 RHI, NVIDIA GPU) takes the UE 5.7+ NVENC-D3D12 ")
            TEXT("path that leaks host memory every encoded frame on recent NVIDIA drivers and ")
            TEXT("eventually OOM-kills the editor. To use hardware H.264 instead, run an engine with ")
            TEXT("the AVCodecs.NvEnc D3D12UsesCUDA backport (ue5-main) and set [AVCodecs.NvEnc] ")
            TEXT("D3D12UsesCUDA=1 in DefaultGame.ini. Details: docs/BUGS.md."),
            *Codec, Ver.GetMajor(), Ver.GetMinor());
#endif // PLATFORM_WINDOWS
    }

    TSharedPtr<FJsonObject> MCPStreamingUnavailableError()
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Pixel Streaming 2 is unavailable — the PixelStreaming2 editor module is not loaded."),
            EMCPErrorCode::FeatureDisabled,
            TEXT("Enable the PixelStreaming2 plugin in the host project (.uproject) and restart the editor. ")
            TEXT("The UnrealMCP plugin declares the dependency, so a stock editor build should have it."));
    }

    // ── Touch-gesture camera control (portable.dev#19) ───────────────────────
    // The phone captures gestures, applies ALL sensitivity/inversion on the
    // mobile side, and sends already-scaled camera deltas as compact JSON over
    // the WebRTC data channel via the PS2 frontend's emitUIInteraction(). We
    // receive them here (see BindCameraControl) and drive the active level-editor
    // viewport free camera. The C++ side is a stable-unit pass-through: look
    // deltas are DEGREES, pan/dolly deltas are WORLD UNITS, scale 1.0 — so feel
    // is tuned entirely by editing the mobile constants + reloading JS (no C++
    // rebuild). Command shapes (tiny, for latency):
    //   {"t":"look","dx":<Δyaw°>,"dy":<Δpitch°>}   turn in place (RMB-look)
    //   {"t":"pan","dx":<ΔRight u>,"dy":<ΔUp u>}    truck/pedestal (no rotation)
    //   {"t":"dolly","d":<ΔForward u>}              dolly along the view forward
    // Point commands (x/y normalized 0..1 in the streamed video CONTENT space —
    // the mobile bridge already unmapped letterbox/crop, and the video == the
    // level-viewport widget backbuffer rect == FViewport size 1:1, no DPI):
    //   {"t":"tap","x":<0..1>,"y":<0..1>}           select the actor under the point
    //   {"t":"focus","x":<0..1>,"y":<0..1>}         select + F-frame the actor
    // Session/view commands:
    //   {"t":"pie","on":<bool>}                     enter/exit Play-In-Editor (in-viewport)
    //   {"t":"viewres","w":<px>,"h":<px>}           phone stream-view size (device px)
    // Orbit (hold-then-drag — swing AROUND a target, vs "look" turning in place):
    //   {"t":"orbitStart","x":<0..1>,"y":<0..1>}    lock the pivot = actor under the point
    //   {"t":"orbit","dx":<Δyaw°>,"dy":<Δpitch°>}   rotate the camera around that pivot
    constexpr double MCPCameraPitchClampDeg = 89.0;

    // Orbit-around-target state (portable.dev#19 usability). A hold-then-drag on the
    // phone sends {t:"orbitStart",x,y} to LOCK a pivot (the actor under the finger,
    // else a point straight ahead), then {t:"orbit",dx,dy} deltas swing the camera
    // AROUND that pivot at constant radius. GAME THREAD ONLY (single viewport, so a
    // file-scope pivot is safe — mirrors the existing static first-cam-log flag).
    FVector GMCPOrbitPivot = FVector::ZeroVector;
    bool GMCPOrbitPivotValid = false;
    constexpr double MCPOrbitMaxPitchDot = 0.985; // ~80° from horizontal — keep off the poles
    constexpr double MCPOrbitEmptyPivotDist = 1000.0; // no actor → pivot this far down the view ray

    /** Apply a parsed camera delta to the active level-editor viewport. GAME THREAD ONLY. */
    /** The level-editor viewport client PS2 is actually STREAMING.
     *
     *  RCA (portable.dev#19): the camera/point handlers previously used the
     *  global GCurrentLevelEditingViewportClient, which the editor only sets
     *  while a viewport has PC-side focus (mouse over / clicked). A streamed
     *  editor sitting untouched on the PC leaves it NULL, so every look/pan/
     *  dolly/tap/focus command silently no-opped — the "gestures do nothing"
     *  bug. GetFirstActiveLevelViewport() is the robust accessor (it's the same
     *  viewport FVideoProducerLevelEditor captures, and the one viewres already
     *  used successfully), so it resolves regardless of PC focus. */
    FLevelEditorViewportClient* MCPGetStreamedLevelViewportClient()
    {
        FLevelEditorModule* LevelEditorModule =
            FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
        if (!LevelEditorModule)
        {
            return nullptr;
        }
        const TSharedPtr<SLevelViewport> LevelViewport = LevelEditorModule->GetFirstActiveLevelViewport();
        if (!LevelViewport.IsValid())
        {
            return nullptr;
        }
        return &LevelViewport->GetLevelViewportClient();
    }

    // ── Smooth camera integration (portable.dev#19 — AAA feel) ──────────────────
    // Phone look/pan/dolly deltas arrive over WebRTC at an IRREGULAR cadence (JS
    // thread + network jitter). Applying each on arrival — and only redrawing then —
    // makes the turn stutter; the PC mouse path feels smooth precisely because it
    // integrates input EVERY frame. So incoming deltas ACCUMULATE here and a
    // per-frame game-thread ticker bleeds them into the camera with a light
    // exponential smoothing (absorbs the jitter at a ~30ms time-constant —
    // imperceptible latency) and redraws every frame while motion is in flight, so
    // the render cadence is steady (frame rate), not packet rate. GAME THREAD ONLY.
    // Orbit stays on the immediate path below (a deliberate, already-slow motion).
    double GMCPPendingLookYaw = 0.0;
    double GMCPPendingLookPitch = 0.0;
    double GMCPPendingPanX = 0.0;
    double GMCPPendingPanY = 0.0;
    double GMCPPendingDolly = 0.0;
    FTSTicker::FDelegateHandle GMCPCameraSmoothTicker;
    constexpr double MCPCameraSmoothTauSec = 0.03; // exponential smoothing time-constant
    constexpr double MCPCameraSettleEps = 1e-4;    // below this the accumulated motion is done

    void MCPTickCameraSmoothing(float DeltaTime)
    {
        const double Mag = FMath::Abs(GMCPPendingLookYaw) + FMath::Abs(GMCPPendingLookPitch)
            + FMath::Abs(GMCPPendingPanX) + FMath::Abs(GMCPPendingPanY) + FMath::Abs(GMCPPendingDolly);
        if (Mag < MCPCameraSettleEps)
        {
            return; // idle — no forced redraw, let the editor sleep
        }
        if (GEditor && GEditor->IsPlaySessionInProgress())
        {
            // The game owns the camera during PIE — drop the accumulated editor motion.
            GMCPPendingLookYaw = GMCPPendingLookPitch = 0.0;
            GMCPPendingPanX = GMCPPendingPanY = GMCPPendingDolly = 0.0;
            return;
        }
        FLevelEditorViewportClient* VC = MCPGetStreamedLevelViewportClient();
        if (!VC)
        {
            return;
        }
        const double Alpha = FMath::Clamp(
            1.0 - FMath::Exp(-static_cast<double>(DeltaTime) / MCPCameraSmoothTauSec), 0.0, 1.0);
        const double DYaw = GMCPPendingLookYaw * Alpha;
        const double DPitch = GMCPPendingLookPitch * Alpha;
        const double DPanX = GMCPPendingPanX * Alpha;
        const double DPanY = GMCPPendingPanY * Alpha;
        const double DDolly = GMCPPendingDolly * Alpha;
        GMCPPendingLookYaw -= DYaw;
        GMCPPendingLookPitch -= DPitch;
        GMCPPendingPanX -= DPanX;
        GMCPPendingPanY -= DPanY;
        GMCPPendingDolly -= DDolly;

        FVector Loc = VC->GetViewLocation();
        FRotator Rot = VC->GetViewRotation();
        if (DYaw != 0.0 || DPitch != 0.0)
        {
            Rot.Yaw += DYaw;
            Rot.Pitch = FMath::Clamp(Rot.Pitch + DPitch, -MCPCameraPitchClampDeg, MCPCameraPitchClampDeg);
            VC->SetViewRotation(Rot);
        }
        if (DPanX != 0.0 || DPanY != 0.0 || DDolly != 0.0)
        {
            const FRotationMatrix RotM(Rot);
            Loc += RotM.GetScaledAxis(EAxis::Y) * DPanX   // Right
                 + RotM.GetScaledAxis(EAxis::Z) * DPanY   // Up
                 + RotM.GetScaledAxis(EAxis::X) * DDolly;  // Forward
            VC->SetViewLocation(Loc);
        }
        VC->Invalidate(false, false);
    }

    void MCPEnsureCameraSmoothTicker()
    {
        if (GMCPCameraSmoothTicker.IsValid())
        {
            return;
        }
        GMCPCameraSmoothTicker = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda([](float Dt) -> bool
            {
                MCPTickCameraSmoothing(Dt);
                return true; // persistent — a cheap no-op when nothing is pending
            }),
            0.0f); // every frame
    }

    // Disable the editor's "throttle CPU when not foreground" while streaming so it
    // keeps ticking + rendering at full rate for the phone even when the PC window is
    // unfocused (portable.dev#19 — wanted regardless of the input-cadence work). The
    // original value is saved and restored when streaming stops. GAME THREAD ONLY.
    bool GMCPThrottleSaved = false;
    bool GMCPThrottleOriginal = true;
    void MCPSetStreamingEditorPerfGameThread(bool bStreaming)
    {
        UEditorPerformanceSettings* Perf = GetMutableDefault<UEditorPerformanceSettings>();
        if (!Perf)
        {
            return;
        }
        if (bStreaming)
        {
            if (!GMCPThrottleSaved)
            {
                GMCPThrottleOriginal = Perf->bThrottleCPUWhenNotForeground;
                GMCPThrottleSaved = true;
            }
            if (Perf->bThrottleCPUWhenNotForeground)
            {
                Perf->bThrottleCPUWhenNotForeground = false;
                Perf->PostEditChange();
                UE_LOG(LogUnrealMCP, Log,
                    TEXT("MCPStreaming: disabled foreground CPU throttle for smooth streaming"));
            }
        }
        else if (GMCPThrottleSaved)
        {
            if (Perf->bThrottleCPUWhenNotForeground != GMCPThrottleOriginal)
            {
                Perf->bThrottleCPUWhenNotForeground = GMCPThrottleOriginal;
                Perf->PostEditChange();
            }
            GMCPThrottleSaved = false;
        }
    }

    void MCPSetStreamingEditorPerf(bool bStreaming)
    {
        if (IsInGameThread())
        {
            MCPSetStreamingEditorPerfGameThread(bStreaming);
        }
        else
        {
            AsyncTask(ENamedThreads::GameThread, [bStreaming]()
            {
                MCPSetStreamingEditorPerfGameThread(bStreaming);
            });
        }
    }

    void MCPApplyCameraDeltaGameThread(const FString& Type, double Dx, double Dy, double D)
    {
        // PIE guard (portable.dev#19 F3): while a PIE session is running (or
        // queued for next tick) the level viewport hosts the GAME —
        // GCurrentLevelEditingViewportClient may be stale/irrelevant and editor
        // camera moves would fight the game camera. The cheap correct answer is
        // to drop look/pan/dolly until PIE ends (phone gestures go inert).
        if (GEditor && GEditor->IsPlaySessionInProgress())
        {
            return;
        }

        // look/pan/dolly → ACCUMULATE for the per-frame smoother (steady, jitter-free
        // motion; see MCPTickCameraSmoothing). No immediate viewport touch/redraw.
        if (Type == TEXT("look"))
        {
            GMCPPendingLookYaw += Dx;
            GMCPPendingLookPitch += Dy;
            MCPEnsureCameraSmoothTicker();
            return;
        }
        if (Type == TEXT("pan"))
        {
            GMCPPendingPanX += Dx;
            GMCPPendingPanY += Dy;
            MCPEnsureCameraSmoothTicker();
            return;
        }
        if (Type == TEXT("dolly"))
        {
            GMCPPendingDolly += D;
            MCPEnsureCameraSmoothTicker();
            return;
        }

        // orbit → immediate (a deliberate slow swing around a locked pivot). The
        // STREAMED level viewport — robust to PC focus (see
        // MCPGetStreamedLevelViewportClient; the old GCurrentLevelEditingViewportClient
        // was null on an untouched streamed editor, which is why gestures did nothing).
        FLevelEditorViewportClient* VC = MCPGetStreamedLevelViewportClient();
        if (!VC)
        {
            return;
        }

        const FVector Loc = VC->GetViewLocation();

        if (Type == TEXT("orbit"))
        {
            // Swing the camera AROUND the locked pivot at constant radius (set by
            // a preceding {t:"orbitStart"}). No valid pivot → no-op.
            if (!GMCPOrbitPivotValid)
            {
                return;
            }
            FVector Offset = Loc - GMCPOrbitPivot;
            if (Offset.IsNearlyZero())
            {
                return;
            }
            // Yaw: rotate the offset around world up (dx degrees).
            Offset = FRotator(0.0, Dx, 0.0).RotateVector(Offset);
            // Pitch: rotate around the current horizontal right axis (dy degrees),
            // but skip the step when it would push the view over the pole so the
            // orbit stays stable near vertical.
            const FVector Right = FVector::CrossProduct(FVector::UpVector, Offset).GetSafeNormal();
            if (!Right.IsNearlyZero())
            {
                const FVector Candidate = Offset.RotateAngleAxis(Dy, Right);
                if (FMath::Abs(FVector::DotProduct(Candidate.GetSafeNormal(), FVector::UpVector)) < MCPOrbitMaxPitchDot)
                {
                    Offset = Candidate;
                }
            }
            const FVector NewLoc = GMCPOrbitPivot + Offset;
            VC->SetViewLocation(NewLoc);
            VC->SetViewRotation((GMCPOrbitPivot - NewLoc).Rotation()); // re-aim at the pivot
        }
        else
        {
            return; // unknown command — ignore, don't invalidate
        }

        // Cheap redraw (no hit-proxy / child-view invalidation) so the moved
        // camera re-renders and the new frame streams to the phone.
        VC->Invalidate(false, false);
    }

    /** Apply a point command (tap-to-select / long-press-to-focus) at normalized
     *  0..1 video-content coords. GAME THREAD ONLY.
     *
     *  Pixel mapping: the streamed video IS the level-viewport widget's
     *  backbuffer rect and FViewport::GetSizeXY() matches it 1:1 in physical
     *  pixels, so pixel = clamp(n * size) with no DPI factor. GetHitProxy
     *  renders the hit-proxy map on demand (game thread); calling Invalidate
     *  frees the returned proxy, so all proxy access happens BEFORE the final
     *  redraw. Selection mirrors the editor's own click path
     *  (FLevelEditorViewportClient::ProcessClick): child actors resolve to
     *  their outermost parent, SelectNone + SelectActor(bNotify=true) fires
     *  NoteSelectionChange (outliner/details stay in sync). */
    /** During an active PIE session, a phone tap must be a real CLICK — synthesized
     *  Slate pointer events at the tap position (they route into UMG widgets AND the
     *  game viewport's player input), never an editor hit-proxy selection. GAME THREAD
     *  ONLY. Maps normalized 0..1 video coords through the streamed level-viewport
     *  widget's cached geometry into absolute desktop coords. */
    void MCPApplyPieClickGameThread(double NX, double NY)
    {
        FLevelEditorModule* LevelEditorModule =
            FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
        if (!LevelEditorModule)
        {
            UE_LOG(LogTemp, Warning, TEXT("MCP PIE tap: LevelEditor module not loaded"));
            return;
        }
        const TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule->GetFirstActiveViewport();
        if (!ActiveViewport.IsValid())
        {
            UE_LOG(LogTemp, Warning, TEXT("MCP PIE tap: no active level viewport"));
            return;
        }

        const FGeometry& Geo = ActiveViewport->AsWidget()->GetCachedGeometry();
        const FVector2D LocalSize = Geo.GetLocalSize();
        if (LocalSize.X <= 0.f || LocalSize.Y <= 0.f)
        {
            UE_LOG(LogTemp, Warning, TEXT("MCP PIE tap: zero-size viewport geometry"));
            return;
        }
        const FVector2D Abs = Geo.LocalToAbsolute(FVector2D(NX * LocalSize.X, NY * LocalSize.Y));

        // The status-bar OUTPUT LOG DRAWER expands OVER the lower viewport (the MCP's
        // own console execs open it), and its list swallows the hit-test — the streamed
        // video shows the game while the desktop hit-test sees the drawer. Dismiss it,
        // and bring the viewport's window to front for good measure.
        if (GEditor)
        {
            if (UStatusBarSubsystem* StatusBar = GEditor->GetEditorSubsystem<UStatusBarSubsystem>())
            {
                StatusBar->ForceDismissDrawer();
            }
        }
        {
            TSharedPtr<SWindow> ViewportWindow =
                FSlateApplication::Get().FindWidgetWindow(ActiveViewport->AsWidget());

            // Transient floating windows ('Message Log' popped by a map check or a
            // live-coding notice) shadow the streamed viewport in SLATE's window order
            // while being invisible on the desktop (behind the main window) — clicks
            // would route into a window the phone viewer cannot see. They can be CHILD
            // windows of the main window (LocateWindowUnderMouse recurses children), so
            // walk the whole tree. Destroy the known notification shadows; the viewer's
            // tap must hit what the stream shows.
            TArray<TSharedRef<SWindow>> Stack =
                FSlateApplication::Get().GetInteractiveTopLevelWindows();
            while (Stack.Num() > 0)
            {
                const TSharedRef<SWindow> Win = Stack.Pop();
                Stack.Append(Win->GetChildWindows());
                if (Win == ViewportWindow)
                {
                    continue;
                }
                const FSlateRect R = Win->GetRectInScreen();
                const bool bContainsPoint =
                    Abs.X >= R.Left && Abs.X <= R.Right && Abs.Y >= R.Top && Abs.Y <= R.Bottom;
                if (!bContainsPoint)
                {
                    continue;
                }
                const FString Title = Win->GetTitle().ToString();
                if (Title == TEXT("Message Log") || Title == TEXT("Output Log"))
                {
                    UE_LOG(LogTemp, Display,
                        TEXT("MCP PIE tap: destroying shadow window '%s' over the streamed viewport"), *Title);
                    Win->RequestDestroyWindow();
                }
                else if (!Title.IsEmpty() && Title != TEXT("FullAutoChess - Unreal Editor") && !Title.Contains(TEXT("Unreal Editor")))
                {
                    UE_LOG(LogTemp, Warning,
                        TEXT("MCP PIE tap: window '%s' overlaps the tap point and may swallow it"), *Title);
                }
            }

            if (ViewportWindow.IsValid())
            {
                ViewportWindow->BringToFront();
            }
        }

        // Diagnostic: what does Slate's hit-test find at that absolute point, and which
        // top-level windows overlap it (a floating Output Log etc. can shadow the game)?
        {
            for (const TSharedRef<SWindow>& Win : FSlateApplication::Get().GetInteractiveTopLevelWindows())
            {
                const FSlateRect Rect = Win->GetRectInScreen();
                if (Abs.X >= Rect.Left && Abs.X <= Rect.Right && Abs.Y >= Rect.Top && Abs.Y <= Rect.Bottom)
                {
                    UE_LOG(LogTemp, Verbose, TEXT("MCP PIE tap: overlapping window '%s' rect=(%.0f,%.0f)-(%.0f,%.0f) visible=%d"),
                        *Win->GetTitle().ToString(), Rect.Left, Rect.Top, Rect.Right, Rect.Bottom,
                        Win->IsVisible() ? 1 : 0);
                }
            }
            FWidgetPath Path = FSlateApplication::Get().LocateWindowUnderMouse(
                Abs, FSlateApplication::Get().GetInteractiveTopLevelWindows());
            FString Breadcrumb;
            for (int32 i = 0; i < Path.Widgets.Num(); ++i)
            {
                Breadcrumb += Path.Widgets[i].Widget->GetTypeAsString();
                if (i < Path.Widgets.Num() - 1) { Breadcrumb += TEXT(" > "); }
            }
            UE_LOG(LogTemp, Verbose, TEXT("MCP PIE tap: n=(%.3f,%.3f) abs=%s window='%s' path=%s"),
                NX, NY, *Abs.ToString(),
                Path.IsValid() ? *Path.GetWindow()->GetTitle().ToString() : TEXT("none"),
                *Breadcrumb);
        }

        FSlateApplication& Slate = FSlateApplication::Get();
        const FVector2D PrevPos = Slate.GetCursorPos();

        // A prior click typically made the game viewport CAPTURE the mouse (default
        // "capture permanently on click"); captured pointers route every subsequent
        // event to the captor, bypassing UMG hit-testing entirely. A phone tap has no
        // notion of an ongoing capture — release it so each tap routes to what the
        // viewer actually sees.
        Slate.ReleaseAllPointerCapture();

        // Move → press → release, exactly like a hardware click at that point.
        Slate.SetCursorPos(Abs);
        FPointerEvent MoveEvent(
            FSlateApplication::CursorPointerIndex, Abs, PrevPos,
            TSet<FKey>(), EKeys::Invalid, 0, FModifierKeysState());
        Slate.ProcessMouseMoveEvent(MoveEvent);

        FPointerEvent DownEvent(
            FSlateApplication::CursorPointerIndex, Abs, Abs,
            TSet<FKey>({ EKeys::LeftMouseButton }), EKeys::LeftMouseButton, 0, FModifierKeysState());
        const bool bDownHandled = Slate.ProcessMouseButtonDownEvent(nullptr, DownEvent);

        FPointerEvent UpEvent(
            FSlateApplication::CursorPointerIndex, Abs, Abs,
            TSet<FKey>(), EKeys::LeftMouseButton, 0, FModifierKeysState());
        const bool bUpHandled = Slate.ProcessMouseButtonUpEvent(UpEvent);
        UE_LOG(LogTemp, Verbose, TEXT("MCP PIE tap: down_handled=%d up_handled=%d"),
            bDownHandled ? 1 : 0, bUpHandled ? 1 : 0);
    }

    void MCPApplyPointCommandGameThread(const FString& Type, double NX, double NY)
    {
        // PIE: taps are clicks (game/UMG input), not editor selection. Long-press
        // "focus" stays editor-only (no-op during PIE rather than a surprise click).
        if (GEditor && GEditor->IsPlaySessionInProgress() && Type == TEXT("tap"))
        {
            MCPApplyPieClickGameThread(NX, NY);
            return;
        }

        FLevelEditorViewportClient* VC = MCPGetStreamedLevelViewportClient();
        if (!VC || !VC->Viewport || !GEditor)
        {
            return;
        }
        const FIntPoint Size = VC->Viewport->GetSizeXY();
        if (Size.X <= 0 || Size.Y <= 0)
        {
            return;
        }
        const int32 HitX = FMath::Clamp(static_cast<int32>(NX * Size.X), 0, Size.X - 1);
        const int32 HitY = FMath::Clamp(static_cast<int32>(NY * Size.Y), 0, Size.Y - 1);

        HHitProxy* Proxy = VC->Viewport->GetHitProxy(HitX, HitY);
        AActor* Actor = nullptr;
        if (Proxy && Proxy->IsA(HActor::StaticGetType()))
        {
            Actor = static_cast<HActor*>(Proxy)->Actor;
            while (Actor && Actor->IsChildActor())
            {
                Actor = Actor->GetParentActor();
            }
        }

        if (Type == TEXT("tap"))
        {
            if (Actor)
            {
                GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true);
                GEditor->SelectActor(Actor, /*bInSelected=*/true, /*bNotify=*/true);
            }
            else if (!Proxy)
            {
                // Tap on empty space/sky clears the selection (backdrop click).
                GEditor->SelectNone(/*bNoteSelectionChange=*/true, /*bDeselectBSPSurfs=*/true);
            }
            // A non-actor proxy (e.g. the transform gizmo's HWidgetAxis) → no-op.
        }
        else if (Type == TEXT("focus"))
        {
            if (Actor)
            {
                // Select first (the editor F-focus ergonomic), then fly the
                // ACTIVE (= streamed) viewport to a good framing of the actor.
                GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true);
                GEditor->SelectActor(Actor, /*bInSelected=*/true, /*bNotify=*/true);
                GEditor->MoveViewportCamerasToActor(*Actor, /*bActiveViewportOnly=*/true);
            }
            // No actor under a long-press → no-op (never yank the camera at the sky).
        }
        else
        {
            return;
        }

        // Done with the proxy — a redraw is now safe (Invalidate may free the
        // hit-proxy map the Proxy pointer came from).
        VC->Invalidate(false, false);
    }

    /** Lock the orbit pivot to the actor under a normalized 0..1 point (the phone's
     *  hold-then-drag start). Reuses the point-command hit-test: child actors
     *  resolve to their outermost parent and the pivot is the actor's BOUNDS
     *  ORIGIN (visual center, so off-origin meshes orbit naturally). No actor under
     *  the finger → pivot a fixed distance straight ahead, so orbit still works on
     *  terrain/empty space. GAME THREAD ONLY. */
    void MCPApplyOrbitStartGameThread(double NX, double NY)
    {
        GMCPOrbitPivotValid = false;
        if (GEditor && GEditor->IsPlaySessionInProgress())
        {
            return; // the game owns the camera during PIE
        }
        FLevelEditorViewportClient* VC = MCPGetStreamedLevelViewportClient();
        if (!VC || !VC->Viewport)
        {
            return;
        }
        const FIntPoint Size = VC->Viewport->GetSizeXY();
        if (Size.X <= 0 || Size.Y <= 0)
        {
            return;
        }
        const int32 HitX = FMath::Clamp(static_cast<int32>(NX * Size.X), 0, Size.X - 1);
        const int32 HitY = FMath::Clamp(static_cast<int32>(NY * Size.Y), 0, Size.Y - 1);

        HHitProxy* Proxy = VC->Viewport->GetHitProxy(HitX, HitY);
        if (Proxy && Proxy->IsA(HActor::StaticGetType()))
        {
            AActor* Actor = static_cast<HActor*>(Proxy)->Actor;
            while (Actor && Actor->IsChildActor())
            {
                Actor = Actor->GetParentActor();
            }
            if (Actor)
            {
                FVector Origin, Extent;
                Actor->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);
                GMCPOrbitPivot = Origin;
                GMCPOrbitPivotValid = true;
            }
        }

        if (!GMCPOrbitPivotValid)
        {
            // No actor → orbit a point straight ahead (terrain/empty-space fallback).
            const FVector Loc = VC->GetViewLocation();
            const FVector Forward = FRotationMatrix(VC->GetViewRotation()).GetScaledAxis(EAxis::X);
            GMCPOrbitPivot = Loc + Forward * MCPOrbitEmptyPivotDist;
            GMCPOrbitPivotValid = true;
        }

        // Done with the proxy — a redraw is now safe (Invalidate may free the map).
        VC->Invalidate(false, false);
    }

    /** Enter/exit Play-In-Editor from the phone ({t:"pie",on}). GAME THREAD ONLY.
     *  Enter replicates the toolbar "Play in viewport" (DebuggerCommands.cpp:1047):
     *  default FRequestPlaySessionParams + DestinationSlateViewport = the first
     *  active level viewport, so PIE runs INSIDE the streamed level-viewport
     *  widget — the single "Editor" streamer keeps showing the game, no second
     *  streamer, no viewer re-subscribe, and our UIInteraction binding stays
     *  live (AutoStreamPIE is off — see HandleStreamStart). Both Request* calls
     *  only QUEUE work for the next editor tick (EditorEngine.h:1783/1840), so
     *  this is cheap; the IsPlaySession* guards make it idempotent. */
    void MCPApplyPieToggleGameThread(bool bOn)
    {
        if (!GEditor)
        {
            return;
        }
        if (bOn)
        {
            if (GEditor->IsPlaySessionInProgress())
            {
                return; // already running or queued — idempotent
            }
            FRequestPlaySessionParams SessionParams;
            if (FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
            {
                const TSharedPtr<IAssetViewport> ActiveLevelViewport = LevelEditorModule->GetFirstActiveViewport();
                if (ActiveLevelViewport.IsValid() && ActiveLevelViewport->GetAssetViewportClient().IsPerspective())
                {
                    SessionParams.DestinationSlateViewport = ActiveLevelViewport;
                }
            }
            // No in-viewport target resolved (module unloaded / ortho viewport):
            // the default params still work — PIE opens per the editor Play
            // settings (usually a new window) and the editor stream continues.
            GEditor->RequestPlaySession(SessionParams);
        }
        else
        {
            if (GEditor->IsPlayingSessionInEditor() || GEditor->IsPlaySessionRequestQueued())
            {
                GEditor->RequestEndPlayMap();
            }
        }
    }

    // ── Phone-aspect editor-window matching ({t:"viewres",w,h}, feature 6) ───
    // The phone reports its stream-view size in DEVICE PIXELS once the stream
    // connects and again on every layout change. PS2 streams the level-viewport
    // WIDGET's on-screen backbuffer rect 1:1 in physical pixels
    // (FVideoProducerLevelEditor), and SetFixedViewportSize would distort (the
    // capture stays widget-shaped), so the real fix is to reshape the OS editor
    // window until the viewport widget approximates the phone aspect. The editor
    // chrome around the viewport (panels/toolbars) is measured live as
    // (window client size − viewport size) and assumed stable across one resize;
    // we keep a height-dominant window (max work-area height) and solve
    //     (ClientW − ChromeW) / (ClientH − ChromeH) == PhoneW / PhoneH.
    // Guards: sane floors, desktop work-area clamp, no-op within tolerance (the
    // phone re-sends viewres liberally — this is the dedupe), and NEVER during
    // PIE (the game's own aspect handling wins). GAME THREAD ONLY.
    constexpr double MCPViewResAspectTolerance = 0.02; // 2% relative — close enough, skip
    constexpr double MCPViewResMinViewportPx = 480.0;  // never squeeze the streamed viewport below this
    constexpr double MCPViewResMinClientPx = 640.0;    // sane minimum window client dimension
    constexpr double MCPViewResTitleBarPx = 48.0;      // work-area allowance for title bar/borders
    constexpr double MCPViewResNoOpPx = 8.0;           // resize deltas below this are noise

    // Outcome of one apply attempt, so the scheduler below can RETRY a transient
    // miss (fresh stream: the viewport/window isn't realized yet at the debounce
    // mark) instead of giving up after a single one-shot — the "wrong res sticks
    // until you reconnect a third time" race. NotReady == transient, retry; the
    // two Done outcomes stop the retry loop.
    enum class EMCPViewResResult : uint8
    {
        Applied,     // window resized — done
        AlreadyClose, // aspect already within tolerance / resize is a no-op — done
        NotReady,     // editor/viewport/window not resolvable yet — retry
    };

    EMCPViewResResult MCPApplyViewResGameThread(int32 PhoneW, int32 PhoneH)
    {
        if (PhoneW < 16 || PhoneH < 16 || !GEditor)
        {
            return EMCPViewResResult::NotReady;
        }
        // Never fight PIE for the viewport shape.
        if (GEditor->IsPlaySessionInProgress())
        {
            return EMCPViewResResult::NotReady;
        }
        if (!FSlateApplication::IsInitialized())
        {
            return EMCPViewResResult::NotReady;
        }

        FLevelEditorModule* LevelEditorModule =
            FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
        if (!LevelEditorModule)
        {
            return EMCPViewResResult::NotReady;
        }
        const TSharedPtr<SLevelViewport> LevelViewport = LevelEditorModule->GetFirstActiveLevelViewport();
        if (!LevelViewport.IsValid())
        {
            return EMCPViewResResult::NotReady;
        }

        // The PS2 producer captures exactly THIS widget's rect, so its
        // FSceneViewport is both the aspect we must match and the window key.
        FLevelEditorViewportClient& Client = LevelViewport->GetLevelViewportClient();
        const TSharedPtr<SEditorViewport> ViewportWidget = Client.GetEditorViewportWidget();
        const TSharedPtr<FSceneViewport> SceneViewport =
            ViewportWidget.IsValid() ? ViewportWidget->GetSceneViewport() : nullptr;
        const TSharedPtr<SWindow> Window =
            SceneViewport.IsValid() ? SceneViewport->FindWindow() : nullptr;
        if (!SceneViewport.IsValid() || !Window.IsValid())
        {
            return EMCPViewResResult::NotReady;
        }

        const FIntPoint ViewportSize = SceneViewport->GetSizeXY();        // physical px
        const FVector2D ClientSize = Window->GetClientSizeInScreen();     // screen-space px
        if (ViewportSize.X <= 0 || ViewportSize.Y <= 0 || ClientSize.X <= 0.0 || ClientSize.Y <= 0.0)
        {
            return EMCPViewResResult::NotReady;
        }

        const double TargetAspect = static_cast<double>(PhoneW) / static_cast<double>(PhoneH);
        const double CurrentAspect = static_cast<double>(ViewportSize.X) / static_cast<double>(ViewportSize.Y);
        if (FMath::Abs(CurrentAspect / TargetAspect - 1.0) <= MCPViewResAspectTolerance)
        {
            return EMCPViewResResult::AlreadyClose; // already close enough — done, swallows re-sends
        }

        // Editor chrome around the viewport widget (assumed stable across one resize).
        const double ChromeW = FMath::Max(0.0, ClientSize.X - static_cast<double>(ViewportSize.X));
        const double ChromeH = FMath::Max(0.0, ClientSize.Y - static_cast<double>(ViewportSize.Y));

        const FSlateRect WorkArea = FSlateApplication::Get().GetWorkArea(Window->GetRectInScreen());
        const double WorkW = WorkArea.GetSize().X;
        const double WorkH = WorkArea.GetSize().Y;

        // Height-dominant: take the tallest sane client height, derive the width.
        double TargetClientH = (WorkH >= MCPViewResMinClientPx)
            ? (WorkH - MCPViewResTitleBarPx)
            : ClientSize.Y;
        double TargetClientW = ChromeW + TargetAspect * (TargetClientH - ChromeH);

        // Width-bound on the desktop? Shrink the height to preserve the aspect.
        if (WorkW >= MCPViewResMinClientPx && TargetClientW > WorkW)
        {
            TargetClientW = WorkW;
            TargetClientH = ChromeH + (TargetClientW - ChromeW) / TargetAspect;
        }

        // Sane floors. A floor can break the exact aspect — acceptable: the phone
        // bridge is letterbox/crop-aware and only needs "close".
        TargetClientW = FMath::Max3(TargetClientW, ChromeW + MCPViewResMinViewportPx, MCPViewResMinClientPx);
        TargetClientH = FMath::Max(TargetClientH, MCPViewResMinClientPx);

        if (FMath::Abs(TargetClientW - ClientSize.X) < MCPViewResNoOpPx &&
            FMath::Abs(TargetClientH - ClientSize.Y) < MCPViewResNoOpPx)
        {
            return EMCPViewResResult::AlreadyClose; // no-op resize — already the right size
        }

        UE_LOG(LogUnrealMCP, Log,
            TEXT("MCPStreaming: viewres %dx%d -> editor window client %.0fx%.0f -> %.0fx%.0f (viewport %dx%d, chrome %.0fx%.0f)"),
            PhoneW, PhoneH, ClientSize.X, ClientSize.Y, TargetClientW, TargetClientH,
            ViewportSize.X, ViewportSize.Y, ChromeW, ChromeH);

        Window->Resize(FVector2D(TargetClientW, TargetClientH));
        return EMCPViewResResult::Applied;
    }

    // ── Deferred, coalesced viewres apply (portable.dev#19 — HARDENED re-enable) ─
    // The resize above touches the SAME editor window PS2 is capturing, so doing it
    // synchronously as the stream goes active deadlocked the encoder (the editor
    // hung with no crash dump right as the stream went active). We therefore NEVER
    // resize inline: a viewres command only STASHES the target + a timestamp, and a
    // low-frequency core ticker applies it after the target has been quiet for
    // MCPViewResDebounceSec, so the resize lands well clear of stream activation and
    // any resize storm (the phone re-sends liberally).
    //
    // The ticker RETRIES on a transient miss (NotReady): a fresh stream often hasn't
    // realized the viewport/window at the debounce mark, and the old one-shot then
    // gave up silently while the phone had already stopped re-sending (video live) —
    // that's the "wrong res sticks until the third reconnect" race. It keeps polling
    // (up to MCPViewResMaxTries) until the resize actually lands (Applied) or the
    // aspect is genuinely already correct (AlreadyClose). GAME THREAD ONLY.
    constexpr double MCPViewResDebounceSec = 2.5; // let the target settle before touching the window
    constexpr int32 MCPViewResMaxTries = 40;      // ~20s of 0.5s polls before giving up on a NotReady
    int32 GMCPPendingViewResW = 0;
    int32 GMCPPendingViewResH = 0;
    int32 GMCPViewResTries = 0;
    double GMCPViewResLastSetSeconds = 0.0;
    FTSTicker::FDelegateHandle GMCPViewResTicker;

    void MCPScheduleViewResGameThread(int32 PhoneW, int32 PhoneH)
    {
        if (PhoneW < 16 || PhoneH < 16)
        {
            return;
        }
        GMCPPendingViewResW = PhoneW;
        GMCPPendingViewResH = PhoneH;
        GMCPViewResLastSetSeconds = FPlatformTime::Seconds();
        GMCPViewResTries = 0; // a fresh target gets a fresh retry budget
        if (GMCPViewResTicker.IsValid())
        {
            return; // already waiting — the refreshed timestamp above extends the debounce
        }
        GMCPViewResTicker = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda([](float) -> bool
            {
                // Still settling (a fresh viewres bumped the timestamp)? keep waiting
                // (doesn't consume the retry budget).
                if (FPlatformTime::Seconds() - GMCPViewResLastSetSeconds < MCPViewResDebounceSec)
                {
                    return true;
                }
                ++GMCPViewResTries;
                const EMCPViewResResult Result =
                    MCPApplyViewResGameThread(GMCPPendingViewResW, GMCPPendingViewResH);
                // Transient (viewport/window not realized yet) → keep polling until
                // it resolves or we exhaust the budget.
                if (Result == EMCPViewResResult::NotReady && GMCPViewResTries < MCPViewResMaxTries)
                {
                    return true;
                }
                GMCPViewResTicker.Reset(); // done (Applied/AlreadyClose) or budget exhausted
                GMCPViewResTries = 0;
                return false;
            }),
            0.5f); // poll twice a second
    }

    /** Parse one camera-command JSON descriptor and dispatch it to the game thread. */
    void MCPDispatchCameraCommand(const FString& Descriptor)
    {
        TSharedPtr<FJsonObject> Obj;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Descriptor);
        if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
        {
            return;
        }

        FString Type;
        if (!Obj->TryGetStringField(TEXT("t"), Type))
        {
            return;
        }

        // Point commands (tap-to-select / long-press-to-focus). x/y are BOTH
        // required — a malformed descriptor must not hit-test at (0,0).
        if (Type == TEXT("tap") || Type == TEXT("focus"))
        {
            double NX = 0.0, NY = 0.0;
            if (!Obj->TryGetNumberField(TEXT("x"), NX) || !Obj->TryGetNumberField(TEXT("y"), NY))
            {
                return;
            }
            NX = FMath::Clamp(NX, 0.0, 1.0);
            NY = FMath::Clamp(NY, 0.0, 1.0);
            if (IsInGameThread())
            {
                MCPApplyPointCommandGameThread(Type, NX, NY);
            }
            else
            {
                AsyncTask(ENamedThreads::GameThread, [Type, NX, NY]()
                {
                    MCPApplyPointCommandGameThread(Type, NX, NY);
                });
            }
            return;
        }

        // {t:"orbitStart",x,y} — lock the orbit pivot to the actor under the point
        // (the phone's hold-then-drag start). x/y BOTH required (never pivot at 0,0).
        if (Type == TEXT("orbitStart"))
        {
            double NX = 0.0, NY = 0.0;
            if (!Obj->TryGetNumberField(TEXT("x"), NX) || !Obj->TryGetNumberField(TEXT("y"), NY))
            {
                return;
            }
            NX = FMath::Clamp(NX, 0.0, 1.0);
            NY = FMath::Clamp(NY, 0.0, 1.0);
            if (IsInGameThread())
            {
                MCPApplyOrbitStartGameThread(NX, NY);
            }
            else
            {
                AsyncTask(ENamedThreads::GameThread, [NX, NY]()
                {
                    MCPApplyOrbitStartGameThread(NX, NY);
                });
            }
            return;
        }

        // {t:"pie",on:<bool>} — enter/exit Play-In-Editor (portable.dev#19 F3).
        // Routed to its own game-thread applier BEFORE the numeric camera-delta
        // parse; RequestPlaySession/RequestEndPlayMap touch editor globals, so
        // the same inline-or-AsyncTask marshal applies.
        if (Type == TEXT("pie"))
        {
            bool bOn = false;
            if (!Obj->TryGetBoolField(TEXT("on"), bOn))
            {
                return;
            }
            if (IsInGameThread())
            {
                MCPApplyPieToggleGameThread(bOn);
            }
            else
            {
                AsyncTask(ENamedThreads::GameThread, [bOn]()
                {
                    MCPApplyPieToggleGameThread(bOn);
                });
            }
            return;
        }

        // {t:"viewres",w,h} — phone stream-view size (device px): resize the
        // editor window so the streamed viewport matches the phone aspect.
        if (Type == TEXT("viewres"))
        {
            double W = 0.0, H = 0.0;
            Obj->TryGetNumberField(TEXT("w"), W);
            Obj->TryGetNumberField(TEXT("h"), H);
            const int32 PhoneW = FMath::RoundToInt32(W);
            const int32 PhoneH = FMath::RoundToInt32(H);
            // Deferred + coalesced (never resize inline — see MCPScheduleViewResGameThread).
            if (IsInGameThread())
            {
                MCPScheduleViewResGameThread(PhoneW, PhoneH);
            }
            else
            {
                AsyncTask(ENamedThreads::GameThread, [PhoneW, PhoneH]()
                {
                    MCPScheduleViewResGameThread(PhoneW, PhoneH);
                });
            }
            return;
        }

        double Dx = 0.0, Dy = 0.0, D = 0.0;
        Obj->TryGetNumberField(TEXT("dx"), Dx);
        Obj->TryGetNumberField(TEXT("dy"), Dy);
        Obj->TryGetNumberField(TEXT("d"), D);

        // The PS2 input handler dispatches messages on its Tick (Slate PreTick =
        // game thread), so we are almost always already on the game thread — apply
        // inline. The AsyncTask branch is a defensive marshal for any future path
        // that fires the handler off-thread.
        if (IsInGameThread())
        {
            MCPApplyCameraDeltaGameThread(Type, Dx, Dy, D);
        }
        else
        {
            AsyncTask(ENamedThreads::GameThread, [Type, Dx, Dy, D]()
            {
                MCPApplyCameraDeltaGameThread(Type, Dx, Dy, D);
            });
        }
    }

    /** The "UIInteraction" data-channel message handler. Decodes the UTF-16LE,
     *  uint16-length-prefixed descriptor string EXACTLY as
     *  FEpicRtcStreamer::OnUIInteraction (EpicRtcStreamer.cpp) does, then
     *  dispatches the camera command. Stateless (operates on the global active
     *  viewport), so it carries no capture and has no lifetime concerns. */
    void MCPCameraUIInteractionHandler(FString /*SourceId*/, FMemoryReader Ar)
    {
        if (Ar.TotalSize() < 2)
        {
            return;
        }
        uint16 StringLength = 0;
        Ar << StringLength;
        if (Ar.TotalSize() < static_cast<int64>(2 + StringLength * sizeof(TCHAR)))
        {
            return;
        }
        FString Descriptor;
        Descriptor.GetCharArray().SetNumUninitialized(StringLength + 1);
        Ar.Serialize(Descriptor.GetCharArray().GetData(), StringLength * sizeof(TCHAR));
        Descriptor.GetCharArray()[StringLength] = TEXT('\0');

        MCPDispatchCameraCommand(Descriptor);
    }

    /** Agent-testable tap: drives the SAME path a phone {t:"tap"} takes (PIE →
     *  synthesized Slate click; editor → hit-proxy select). Also the sanctioned
     *  workaround for GAP-030 (raw pie_send_mouse doesn't actuate Slate UI).
     *  Usage: MCP.Stream.Tap <nx 0..1> <ny 0..1> */
    static FAutoConsoleCommand GMCPStreamTapCmd(
        TEXT("MCP.Stream.Tap"),
        TEXT("Synthesize a viewer tap at normalized viewport coords (PIE: real Slate click; editor: tap-select). Usage: MCP.Stream.Tap 0.5 0.58"),
        FConsoleCommandWithArgsDelegate::CreateStatic(
            [](const TArray<FString>& Args)
            {
                if (Args.Num() < 2)
                {
                    return;
                }
                const double NX = FCString::Atod(*Args[0]);
                const double NY = FCString::Atod(*Args[1]);
                if (IsInGameThread())
                {
                    MCPApplyPointCommandGameThread(TEXT("tap"), NX, NY);
                }
                else
                {
                    AsyncTask(ENamedThreads::GameThread, [NX, NY]()
                    {
                        MCPApplyPointCommandGameThread(TEXT("tap"), NX, NY);
                    });
                }
            }));
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

    // Safe-by-default for stock 5.7 engines: force VP8 when this process would take
    // the leaking NVENC-D3D12 path (runs BEFORE the already-streaming early return so
    // late joiners on a live stream negotiate the safe codec too).
    MCPApplyNvEncD3D12LeakGuard();

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

    // Configure the editor-streaming CVars: stream the level viewport and use
    // the EMBEDDED signalling server. AutoStreamPIE is OFF on purpose
    // (portable.dev#19 F3): the phone's {t:"pie",on} command starts PIE IN the
    // level viewport (DestinationSlateViewport), so the single "Editor"
    // streamer already shows the game; a second PIE streamer ("DefaultStreamer")
    // would fork the viewer choice and lack our UIInteraction binding.
    MCPSetPS2CVar(TEXT("PixelStreaming2.Editor.Source"), TEXT("LevelEditorViewport"));
    MCPSetPS2CVar(TEXT("PixelStreaming2.Editor.AutoStreamPIE"), false);
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

    // Bind touch-gesture camera control now (the input handler exists once the
    // streamer does) and again on each (re)start below, since a streamer restart
    // rebuilds its input handler and drops our message handler.
    BindCameraControl(EditorStreamer.Get());

    // The delegates may fire off the game thread; SetStreamState is lock-guarded
    // and the lambdas re-guard bridge lifetime with a weak pointer, so this is
    // safe from any thread. They only patch the cached flags — no PS2 queries.
    TWeakObjectPtr<UMCPBridge> WeakBridge(Bridge);
    EditorStreamer->OnStreamingStarted().AddLambda([WeakBridge](IPixelStreaming2Streamer* Streamer)
    {
        // Re-bind camera control: a (re)start rebuilds the input handler, so our
        // "UIInteraction" message handler must be re-registered (idempotent —
        // it just overwrites the id-50 dispatch slot). Static (no member access)
        // so it is safe even if the owning handler were gone.
        FMCPStreamingCommands::BindCameraControl(Streamer);
        // Keep the editor rendering at full rate for the phone while streaming.
        MCPSetStreamingEditorPerf(true);
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
            // Restore the throttle once no streamers remain.
            if (!State.bActive)
            {
                MCPSetStreamingEditorPerf(false);
            }
        }
    });
    bStreamerDelegatesBound = true;
}

void FMCPStreamingCommands::BindCameraControl(IPixelStreaming2Streamer* Streamer)
{
    if (!Streamer)
    {
        return;
    }
    // GetInputHandler is a weak pointer — the handler is owned by the streamer.
    // Registering "UIInteraction" overwrites the streamer's default handler
    // (which broadcasts to blueprint UPixelStreaming2Input components); the
    // editor streamer has no such components, so replacing it is safe and gives
    // us the raw descriptor for direct camera control.
    if (const TSharedPtr<IPixelStreaming2InputHandler> Input = Streamer->GetInputHandler().Pin())
    {
        Input->RegisterMessageHandler(TEXT("UIInteraction"), &MCPCameraUIInteractionHandler);
    }
}
