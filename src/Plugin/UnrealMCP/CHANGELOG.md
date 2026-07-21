# Changelog — UnrealMCP plugin

Notable changes to the UE editor plugin. Newest first; add an entry in the same
change that alters behavior.

## Unreleased

- **Synthesized PIE drag-and-drop (portable.dev#19).** New `MCP.Stream.Drag <nx0>
  <ny0> <nx1> <ny1> [seconds=0.35]` console command drives a viewer drag through the
  same synthesized-Slate path as `MCP.Stream.Tap`: press → per-frame interpolated
  mouse-moves with the left button HELD → release. The held-move stream is what lets
  UMG's `DetectDragIfPressed`/`OnDragDetected`/`OnDrop` machinery fire, so games using
  standard UMG drag-and-drop get working drags with zero extra wiring. Game-thread
  only, one drag at a time; same drawer/pointer-capture hygiene as the tap. Also adds a
  raw touch-phase wire path — `{t:"touch",ph:"s"|"m"|"e",x,y}` (start/move/end) — so a
  real finger drag becomes a real Slate drag once the Portable client ships touch
  phases; ignored outside PIE. `x`/`y` are both required so a malformed descriptor
  never presses at (0,0).
- **Touch-gesture editor camera control (portable.dev#19).** `MCPStreamingCommands`
  handles `emitUIInteraction` camera commands from the phone over the PS2 data
  channel: `look`/`pan`/`dolly` (drag/pinch), `orbit`+`orbitStart` (hold-then-drag
  around a hit-tested pivot), `tap`/`focus` (hit-test select / F-frame), `pie`, and
  `viewres`. Applied to the STREAMED level viewport via
  `GetFirstActiveLevelViewport()` — **not** `GCurrentLevelEditingViewportClient`,
  which is null on an untouched (unfocused) streamed editor and silently no-opped
  every gesture (the "gestures do nothing" RCA). Full reference:
  [`docs/EDITOR-STREAMING.md`](../../../docs/EDITOR-STREAMING.md).
- **Smooth camera integration (AAA feel).** look/pan/dolly no longer apply on packet
  arrival (irregular WebRTC/JS cadence → stutter); they accumulate and a per-frame
  `FTSTicker` bleeds them into the camera with light exponential smoothing (~30ms)
  and redraws every frame while in motion — steady render cadence, not packet cadence.
  Idle → no forced redraw. (`MCPTickCameraSmoothing`.)
- **Phone-aspect editor-window matching (`{t:"viewres",w,h}`), HARDENED.** The reshape
  is `SWindow::Resize` on the same window PS2 captures; applying it INLINE as the
  stream went active deadlocked the encoder (editor hung, no crash dump). Now it is
  never inline: `MCPScheduleViewResGameThread` defers + debounces (~2.5s quiet) and
  **retries until resolved** (`EMCPViewResResult`) — a fresh stream isn't realized at
  the debounce mark, and the old one-shot gave up there (the "wrong res until the
  third reconnect" race). Never during PIE; no-op within 2% aspect tolerance.
- **Full-rate rendering while streaming.** `OnStreamingStarted` disables
  `UEditorPerformanceSettings::bThrottleCPUWhenNotForeground` (restored on stop) so the
  editor keeps ticking/rendering for the phone when the PC window is unfocused.

- **Pixel Streaming 2 control (portable.dev#19 M2).** New
  `Commands/MCPStreamingCommands.(h|cpp)` handler + bridge dispatch:
  `stream_start` {viewer_port?=8890, streamer_port?=8888} sets the PS2 editor
  CVars (Source=LevelEditorViewport, AutoStreamPIE=true,
  UseRemoteSignallingServer=false), configures the embedded signalling server
  (SetViewerPort FIRST — the Windows default is 80 — then SetStreamerPort +
  SetSignallingDomain ws://127.0.0.1) and calls
  IPixelStreaming2EditorModule::StartStreaming(LevelEditorViewport). Async by
  nature (first launch may download the signalling-server bundle) — success
  means `state:"starting"`; idempotent when already streaming. `stream_stop`
  is idempotent; `stream_status` reports {active, viewer_port, streamer_port,
  streamers[]}. Degrades to `feature_disabled` when the PS2 modules are
  unavailable. Both mutators joined the dry-run blocklist; deliberately NOT
  PIE-blocked (AutoStreamPIE hands the stream to the PIE viewport).
- `mcp_status` now embeds `stream: {active, viewer_port, streamers}` read from
  a lock-guarded cache on the bridge (written on the game thread by the
  stream handlers + the editor streamer's OnStreamingStarted/Stopped
  delegates) — the network-thread status path never queries the PS2 modules.
- UnrealMCP now declares a hard dependency on the **PixelStreaming2** plugin
  (uplugin ref + PixelStreaming2Editor/PixelStreaming2/PixelStreaming2Core/
  PixelStreaming2Settings/PixelStreaming2Servers module deps) — this
  force-enables PS2 for host projects that load UnrealMCP; accepted for the
  M2 PoC.

- `asset_textures_import`: optional per-image `settings.lod_group` (TEXTUREGROUP_*)
  and `settings.mip_gen` (TMGS_*) applied post-import alongside sRGB/compression —
  only when sent, so factory defaults (World / FromTextureGroup) otherwise stand.
  Unknown literals warn and fall back. Result entries now report `lod_group` +
  `mip_gen`.
- `physics_material_create` (asset factory): creates a `UPhysicalMaterial` via
  `UPhysicalMaterialFactoryNew` with friction / static friction / restitution
  (validated 0..1) / density, plus optional `friction_combine_mode` /
  `restitution_combine_mode` (Average|Min|Multiply|Max — passing one also sets
  the matching `bOverride*CombineMode` flag, without which the project default
  wins). Auto-saves; `name_collision` on existing path; added to the dry-run
  and PIE blocklists.
- Armed auto-record (`pie_record_arm`/`pie_record_disarm`): while armed, every
  PIE session records itself — capture starts on `PostPIEStarted` (with a
  bounded present-retry, so the opening seconds are never missed) and
  finalizes on PIE end / per-take cap. Takes auto-number `<base>_NN.mp4`;
  arming mid-session latches onto the running session.
- PIE recordings now capture the game audio the player hears: an
  `ISubmixBufferListener` on the PIE audio device's MAIN submix (final mix,
  post-effects) feeds a second AAC-LC 192kbps stereo stream on the same MP4
  sink writer. Video and audio share one timeline (t=0 = recording start);
  audio anchors at its first callback then advances by accumulated sample
  count — gapless and drift-free. >2-channel mixes downmix to stereo;
  `audio:false`, a missing audio device, or an AAC-less system degrade to
  video-only (`result.audio` / `audio_note`), never a failed recording.
- PIE video recording (`pie_record_start`/`stop`/`status`): records the live
  PIE viewport — UMG/HUD composited in — to an H.264 MP4 entirely in-engine
  (FFrameGrabber back-buffer capture at native viewport resolution → dedicated
  encoder thread → Media Foundation sink writer with hardware MFTs, software
  fallback, and encoder-side downscale). PIE-only, one recording at a time,
  hard `max_duration_s` watchdog, auto-finalize on PIE end, frame-drop (never
  game-thread stall) under backpressure. Refused under `-nullrhi`
  (`feature_disabled`) and for `dry_run`. Windows-only.
