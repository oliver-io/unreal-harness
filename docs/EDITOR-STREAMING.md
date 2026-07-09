# Editor Streaming & Touch-Gesture Camera Control (portable.dev#19)

End-to-end reference for streaming a live Unreal **editor** viewport to the Portable
mobile app over Pixel Streaming 2 (PS2) and driving its camera with phone touch
gestures. This is the single source of truth for the *working feature*; the `tmp/fleet/*`
planning docs in the mobile repo are historical scratch and are superseded by this.

> **Making this engine-agnostic?** The plan to decouple Unreal/PS2 behind a reusable
> "stream-source provider" contract (so any engine/app can plug into Portable — core
> already has zero MCP coupling) lives in the mobile repo:
> `mobile-vgit/docs/STREAM-SOURCE-PROTOCOL.md`. This doc is the current Unreal-only
> reference implementation of that (future) protocol.

The feature spans **two repos** — each section labels which:

- **[engine]** `unreal/mcp` — the UnrealMCP plugin
  (`Source/UnrealMCP/Private/Commands/MCPStreamingCommands.cpp`).
- **[mobile]** `mobile-vgit` — the Expo RN app
  (`packages/mobile/src/features/stream/{StreamPlayerView,EngineStreamOverlay}.tsx`).

---

## 1. The pipeline

```
[mobile] EngineStreamOverlay (fullscreen Modal)
   └─ StreamPlayerView: WebView loads the PS2 player page (…/api/stream/?ss=…&AutoConnect=true)
        │  transparent react-native-gesture-handler layer OVER the video (pointerEvents="none")
        │  gestures → window.__cam({...}) / window.__camPoint(kind,x,y)  [injected page bridge]
        ▼  emitUIInteraction → WebRTC data channel
[engine] PS2 editor streamer → "UIInteraction" message handler (MCPCameraUIInteractionHandler)
        │  BindCameraControl re-registers it on every OnStreamingStarted
        ▼  MCPDispatchCameraCommand (JSON) → game-thread appliers
     the STREAMED level-editor viewport (MCPGetStreamedLevelViewportClient)
```

Detection (is an editor up?) is a separate chain: editor `MCPBridge` (TCP 55557) ←
harness `bun run mcp` (`GET /status` on 127.0.0.1:8765) ← Portable api `EngineStateService`
→ `user:runtime_state` → the app's UE badge. See the harness repo for that half.

---

## 2. Wire contract — `emitUIInteraction` commands

All camera commands ride the PS2 data channel as JSON objects with a `t` discriminator.
Parsed in `MCPDispatchCameraCommand`; deltas are pre-scaled on the phone (degrees for
look/orbit, world units for pan/dolly) so the engine applies stable units.

| Command | Payload | Meaning |
| --- | --- | --- |
| `look` | `{dx,dy}` | Δyaw°, Δpitch° (1-finger drag). **Accumulated + smoothed.** |
| `pan` | `{dx,dy}` | ΔRight, ΔUp world units (2-finger drag). **Accumulated + smoothed.** |
| `dolly` | `{d}` | ΔForward world units (2-finger spread past a dead-zone). **Accumulated + smoothed.** |
| `orbit` | `{dx,dy}` | Δyaw°, Δpitch° around a locked pivot (hold-then-drag). Immediate. |
| `orbitStart` | `{x,y}` | Lock the orbit pivot to the actor under normalized 0..1 point. |
| `tap` | `{x,y}` | Hit-test + select the actor under 0..1 point (empty → clear). |
| `focus` | `{x,y}` | Hit-test + F-frame the actor (long-press). |
| `pie` | `{on}` | Enter/exit Play-In-Editor. |
| `viewres` | `{w,h}` | Phone stream-view size in DEVICE PIXELS → match editor aspect. |

`__camPoint(kind,fx,fy)` (page bridge) converts a WebView-fraction touch into VIDEO-CONTENT
0..1 coords — letterbox/crop-aware via the `<video>` rect + computed `object-fit` — so tap/
focus/orbitStart map correctly under either `cover` or `contain`.

---

## 3. Camera control — engine [engine]

### 3.1 Which viewport (the "gestures do nothing" RCA)

`MCPGetStreamedLevelViewportClient()` resolves the streamed viewport via
`FLevelEditorModule::GetFirstActiveLevelViewport()` — **NOT** the global
`GCurrentLevelEditingViewportClient`. The editor only sets that global while a viewport
has PC-side focus (mouse-over/click); a streamed editor sitting untouched on the PC leaves
it `null`, so every look/pan/dolly/tap/focus silently no-opped — the original
"gestures do nothing" bug. `GetFirstActiveLevelViewport()` is the same widget PS2 captures
and resolves regardless of PC focus.

### 3.2 Smooth integration (the AAA-feel fix)

**Symptom:** phone-driven turns felt stuttery/jerky (observable on both the phone and the
PC screen, even with the PC window focused) while PC-mouse turns were smooth.

**Root cause:** phone deltas arrive over WebRTC at an *irregular* cadence (JS-thread +
network jitter), and the old code applied each delta the instant it arrived and only
redrew then — so the motion (and the render cadence) followed packet timing. The PC mouse
path feels smooth because the editor integrates input *every frame*.

**Fix** (`MCPApplyCameraDeltaGameThread` → `MCPTickCameraSmoothing` / `MCPEnsureCameraSmoothTicker`):
look/pan/dolly no longer apply on arrival — they **accumulate** into `GMCPPendingLook*/Pan*/Dolly`.
A persistent per-frame game-thread `FTSTicker` (period `0.0f`) bleeds the accumulators into
the camera with light exponential smoothing (`Alpha = 1 - exp(-dt/tau)`, `tau = 30ms` —
imperceptible latency) and calls `Invalidate(false,false)` **every frame while motion is in
flight**, so the render cadence is steady (frame rate), not packet rate. Idle (accumulators
< `MCPCameraSettleEps`) → no forced redraw, the editor sleeps. Dropped during PIE.

Orbit stays on the immediate path (a deliberately slow swing around a locked pivot; not
latency-sensitive).

### 3.3 Foreground CPU throttle

While streaming, `MCPSetStreamingEditorPerf(true)` (from `OnStreamingStarted`) disables
`UEditorPerformanceSettings::bThrottleCPUWhenNotForeground` so the editor keeps ticking +
rendering at full rate for the phone even when the PC window is unfocused. The original
value is saved and restored on `OnStreamingStopped` when no streamers remain.

---

## 4. Camera control — mobile [mobile]

### 4.1 RNGH inside the overlay Modal (dead-gesture fix)

`EngineStreamOverlay` is a core RN `<Modal>` = a **separate native Android window** that
sits OUTSIDE the app-root `GestureHandlerRootView` (`app/_layout.tsx`). react-native-gesture-
handler gets **no touches** inside it — so the camera gesture layer AND the composer drag
were silently dead while native-touch widgets (PIE button, text input, drawing WebView) still
worked. **Fix:** the Modal wraps its content in its own `<GestureHandlerRootView>`.

### 4.2 Gesture composition (`StreamPlayerView`)

`Gesture.Simultaneous(pinch, twoPan, Gesture.Exclusive(holdFamily, tap, lookPan))`, where
`holdFamily = Gesture.Simultaneous(focus, orbit)`. An immediate 1-finger drag → **look**; a
2-finger drag → **pan**; a pinch → **dolly**; a quick tap → **select**; a long-press → **focus**
(zoom-to), and *keep holding + drag* → **orbit** around that target (`Pan().activateAfterLongPress`).
All feel/inversion lives in the exported constants at the top of the file (edit + reload JS,
no C++ rebuild).

### 4.3 Steady emit pump (cadence fix, pairs with §3.2)

Gestures do NOT emit directly and do NOT use a per-`onUpdate` `setTimeout(16)` flush (RNGH
`onUpdate` fires irregularly on the JS thread → coarse, jittery packet cadence). Instead they
**accumulate** into `pending`, and a single persistent `requestAnimationFrame` pump drains it
into exactly one packet per channel each frame (~60Hz, frame-aligned). The pump also generates
the continuous two-finger forward dolly from the live pinch spread (`dollyScale`). One clock
for all motion; the engine smooths the rest.

---

## 5. Aspect matching — `{t:"viewres"}`

**Problem:** the editor renders landscape (~16:9); the phone is portrait. Phone-side
`object-fit:cover` fills the screen but center-crops away most of the editor's width (you see
a narrow strip). Matching the editor's render aspect to the phone is the real fix.

### 5.1 Engine [engine] — reshape the editor window, HARDENED

PS2 captures the level-viewport widget's on-screen backbuffer 1:1; `SetFixedViewportSize`
distorts (capture stays widget-shaped), so the real lever is reshaping the OS editor window
until the viewport widget approximates the phone aspect (`MCPApplyViewResGameThread`, solving
`(ClientW−ChromeW)/(ClientH−ChromeH) == PhoneW/PhoneH`, height-dominant, work-area clamped).

This is `SWindow::Resize` on the **same window PS2 is capturing**. Doing it **inline** as the
stream went active **deadlocked the encoder** (the editor hung with no crash dump). So it is
**never applied inline**:

- **Deferred + coalesced** (`MCPScheduleViewResGameThread`): a viewres command only stashes the
  target + a timestamp; a low-frequency ticker applies it only after the target has been quiet
  for `MCPViewResDebounceSec` (~2.5s) — clear of stream activation and the phone's re-send storm.
- **Retry-until-resolved** (`EMCPViewResResult` — `Applied` / `AlreadyClose` / `NotReady`): a
  fresh stream often hasn't realized the viewport/window at the debounce mark. The old one-shot
  gave up silently there (while the phone had stopped re-sending because video was live) — that
  was the **"wrong res sticks until the third reconnect"** race. The ticker now retries on
  `NotReady` (up to `MCPViewResMaxTries` ≈ 20s) until the resize actually lands.
- Never during PIE; no-op within a 2% aspect tolerance (swallows re-sends).

### 5.2 Mobile [mobile]

`sendViewres` injects `window.__viewres(w,h)` in device pixels (`onLayout` × `PixelRatio`,
deduped; re-sent on `onLoadEnd` — a pre-load injection dies with the old JS context). The page
bridge (`ASPECT_BRIDGE_JS`) retries the emit every 1.5s until the data channel is live, then
stops; it also owns the `object-fit:cover` skin + `window.__setFit('cover'|'contain')` (PIE
flips to `contain` so the game aspect letterboxes instead of cropping). Once the engine reshape
lands, `cover` fills exactly (no crop, no bars).

---

## 6. Stream readiness — 503 auto-reload [mobile]

The overlay loads the WebView as soon as it has a `playerUrl`, but the PC's PS2 player HTTP
endpoint may not be serving yet (stream still spinning up) → the page loads a **5xx/503** error
doc with no `window.pixelStreaming`, so nothing works. Before this fix you had to leave the
overlay and return. Now `StreamPlayerView` reacts to a main-frame 5xx (`onHttpError`) by
remounting the WebView (`webviewEpoch` key) on a capped backoff (0.8s→5s, ≤12 tries) until a
real page loads; a clean `onLoadEnd` refreshes the budget so a later transient drop can recover.

---

## 7. Footgun registry

- **External-plugin stale DLL after a build.** UnrealMCP is loaded via
  `AdditionalPluginDirectories` (not under the project's `Plugins/`). `scripts/build-editor.ps1`
  compiles it, but UBT writes the fresh `UnrealEditor-UnrealMCP.dll` into the **project** Binaries
  while the editor LOADS it from the **plugin's own** Binaries. The script's `Sync-ExternalPluginBinaries`
  silently skips when the plugin Binaries has no `UnrealEditor.modules` (which it doesn't), so the
  editor keeps loading the STALE DLL. **A "successful build" then runs old code** (this is why an
  orbit build once "did nothing"). **After every build, manually copy** the fresh `.dll`+`.pdb` from
  `MedievalCS/Binaries/Win64` → `mcp/src/Plugin/UnrealMCP/Binaries/Win64` and verify the timestamps
  match before relaunching. See also `docs/DEBUGGING.md`.
- **`GCurrentLevelEditingViewportClient` is null on an untouched streamed editor** → use
  `GetFirstActiveLevelViewport()` (§3.1).
- **RNGH is dead inside a core RN Modal** → give the Modal its own `GestureHandlerRootView` (§4.1).
- **Resizing the captured editor window inline deadlocks the encoder** → defer + debounce + retry (§5.1).
- **One-shot viewres apply loses the race on a fresh stream** → retry-until-resolved (§5.1).
- **`setTimeout(16)`-per-`onUpdate` emit is jittery** → steady rAF pump + engine per-frame smoothing (§4.3/§3.2).
- **The three-state connect sequence** (503 → wrong-res → correct across three reconnects) had TWO
  root causes, now both fixed: the 503 (§6) and the one-shot viewres race (§5.1).

---

## 8. Rebuild + deploy loop (quick reference)

1. Close the editor (DLL lock).
2. `UNREAL_ENGINE_ROOT=… UNREAL_PROJECT_ROOT=…/MedievalCS.uproject
   powershell -File scripts/build-editor.ps1` (incremental; ~40–90s).
3. **Manually sync** the fresh DLL+PDB into the plugin Binaries (§7, first bullet).
4. Relaunch the editor; harness `/status` → `editorUp:true` confirms the module loaded.
5. Mobile: edit `StreamPlayerView.tsx` / reload the JS bundle — **no C++ rebuild needed for
   feel/inversion tuning** (all in the exported constants). C++ rebuild only for engine behavior.

---

## 9. Making a project streamable from Portable (onboarding checklist)

What a fresh Unreal project needs to be tap-to-streamable — this is the recipe to lift into
the public docs. Verified end-to-end on `unreal/mobile-game` (FullAutoChess) on 2026-07-08.

**A. Project `.uproject` — two plugins:**

- **UnrealMCP** (via `AdditionalPluginDirectories` → the harness plugin path, e.g.
  `C:/Users/olive/code/unreal/mcp/src/Plugin`; never copied). `TargetAllowList: ["Editor"]`.
- **PixelStreaming2**, `TargetAllowList: ["Editor"]`. UnrealMCP's `.uplugin` already declares
  PS2 as a dependency, so it loads transitively — but **list it explicitly** anyway: it's
  self-documenting and matches the proven reference (MedievalCS). If PS2 isn't loaded,
  `stream_start` fails with *"Enable the PixelStreaming2 plugin in the host project (.uproject)
  and restart the editor."*

An already-running editor picks up a newly-added explicit PS2 entry only on **restart**; but if
UnrealMCP is present, the transitive dep means the current session can usually already stream
(verify with the smoke test below before assuming a restart is needed).

**B. Harness running** — `bun run mcp` in `unreal/mcp/` listens on loopback `127.0.0.1:8765`
(`UNREAL_MCP_PORT`) and bridges to the running editor over TCP **55557** (MCPBridge). It serves
`GET /status`, `POST /control/stream/{start,stop}`. It talks to whatever editor is up — no
per-project config.

**C. Portable api — all defaults, no config needed** if the harness is on 8765:
`PORTABLE_ENGINE_STATUS_URL` = `http://127.0.0.1:8765/status`, `PORTABLE_ENGINE_CONTROL_URL`
= `http://127.0.0.1:8765`, `PORTABLE_STREAM_VIEWER_PORT` = `8890`. The api polls `/status` →
UE badge → tap → `POST /control/stream/start` → reverse-proxies viewer **8890** through the relay.

**Ports:** 8765 harness (status/control/mcp) · 55557 editor TCP bridge · 8890 PS2 viewer/HTTP ·
8888 PS2 streamer WS.

**D. Forward-looking config artifact — `portable.json` at the project root.** Not consumed yet
(Phase 2 of `mobile-vgit/docs/STREAM-SOURCE-PROTOCOL.md` §4), but authored now as the durable,
machine-readable declaration of the source (kind/control/player/input/match). It replaces the
hardcoded env defaults once the manifest reader lands. See `unreal/mobile-game/portable.json`.

**Smoke test (no phone needed):**
```bash
curl -s http://127.0.0.1:8765/status                       # editorUp:true, project:"<name>"
curl -s -X POST http://127.0.0.1:8765/control/stream/start -d '{"viewerPort":8890}'  # {ok:true,...}
curl -s http://127.0.0.1:8765/status                       # stream.active:true, streamers:["Editor"]
curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8890/   # 200 = player page serving
curl -s -X POST http://127.0.0.1:8765/control/stream/stop  # {ok:true}
```
