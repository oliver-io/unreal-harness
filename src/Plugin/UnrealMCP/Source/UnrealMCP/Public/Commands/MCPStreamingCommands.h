#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class UMCPBridge;
class IPixelStreaming2Streamer;

/**
 * Snapshot of the editor's Pixel Streaming 2 state, cached on the bridge so
 * `mcp_status` — answered synchronously on the NETWORK thread — never has to
 * touch the PS2 modules (whose accessors are game-thread shaped). Written on
 * the game thread by the stream_* handlers (and by the editor streamer's
 * OnStreamingStarted/Stopped delegates); read under the bridge's lock.
 */
struct FMCPStreamState
{
    /** True while at least one Pixel Streaming streamer is live. */
    bool bActive = false;
    /** True once a stream handler has observed a concrete viewer port. */
    bool bViewerPortKnown = false;
    /** Signalling-server viewer (HTTP) port; meaningful only when bViewerPortKnown. */
    int32 ViewerPort = 0;
    /** IDs of the currently-streaming streamers (e.g. "Editor"). */
    TArray<FString> Streamers;
};

/**
 * Pixel Streaming 2 control — stream the level editor viewport to a browser
 * via PS2's embedded signalling server (portable.dev#19 M2 PoC).
 *
 * Surface:
 *   - stream_start  {viewer_port?=8890, streamer_port?=8888} — configure CVars +
 *     ports, then IPixelStreaming2EditorModule::StartStreaming(LevelEditorViewport).
 *     Async by nature (the first launch may download the signalling-server
 *     bundle), so success means "starting". Idempotent while already streaming.
 *   - stream_stop   — StopStreaming(); idempotent (success when nothing runs).
 *   - stream_status — {active, viewer_port, streamer_port, streamers[]}.
 *
 * All handlers run on the GAME thread (bridge dispatch). Each one refreshes the
 * bridge's cached FMCPStreamState so mcp_status can answer from the network
 * thread without querying PS2. Degrades cleanly (feature_disabled / inert
 * status) when the PixelStreaming2 plugin or its modules are unavailable.
 *
 * NOT in the PIE blocklist on purpose: streaming must keep working during a
 * Play-In-Editor session. AutoStreamPIE is deliberately OFF (stream_start sets
 * the CVar false): the phone's {t:"pie",on} UIInteraction command starts PIE
 * IN the level viewport (DestinationSlateViewport), so the single "Editor"
 * streamer keeps showing the game and the UIInteraction binding stays live —
 * a second PIE streamer ("DefaultStreamer") would fork viewers and lack it.
 */
class FMCPStreamingCommands
{
public:
    explicit FMCPStreamingCommands(UMCPBridge* InBridge);

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleStreamStart(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleStreamStop(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleStreamStatus(const TSharedPtr<FJsonObject>& Params);

    /** Recompute FMCPStreamState from the live PS2 modules (game thread) and
     *  push it into the bridge cache. Safe no-op when PS2 is unavailable. */
    void RefreshStreamStateCache();

    /** Bind (once) to the editor streamer's OnStreamingStarted/Stopped events so
     *  the cache also tracks stops that don't come through stream_stop. Also binds
     *  the touch-gesture camera-control handler on the streamer's input handler. */
    void EnsureStreamerDelegatesBound();

    /** Register the "UIInteraction" data-channel message handler on the given
     *  streamer's input handler, so mobile camera-delta commands drive the active
     *  level-editor viewport free camera (portable.dev#19 touch camera control).
     *  Static + stateless (drives the global active viewport) so it is safe to
     *  call from the streamer's OnStreamingStarted delegate. Idempotent. */
    static void BindCameraControl(IPixelStreaming2Streamer* Streamer);

    /** Owning bridge (the handler is a TSharedPtr member of the bridge, so the
     *  raw back-pointer cannot outlive it; delegate lambdas re-guard with a
     *  TWeakObjectPtr instead). */
    UMCPBridge* Bridge = nullptr;

    bool bStreamerDelegatesBound = false;
};
