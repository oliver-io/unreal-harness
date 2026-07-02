# Changelog — UnrealMCP plugin

Notable changes to the UE editor plugin. Newest first; add an entry in the same
change that alters behavior.

## Unreleased

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
