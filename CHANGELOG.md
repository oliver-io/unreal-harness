# Changelog — MCP server (`src/server/`)

Notable changes to the Bun/TypeScript MCP server. Newest first; add an entry in the
same change that alters behavior.

## Unreleased

- **Test-harness project-identity guard.** Editor-gated tests can no longer run
  against a project the harness doesn't own: the Bun `editorSuite` now verifies the
  attached editor's `project_context` (settings_paths[0]) matches the test project
  (`UE_MCP_TEST_PROJECT`, default the fixture) and skips otherwise; pytest
  `--ue-attach` refuses with the same check. Previously the suites attached blindly
  to whatever interactive editor was on :55557 — which is how a real game
  (projects/trong) accumulated `__MCPTest__` debris and two in-place asset
  mutations. `UE_MCP_ATTACH_ANY=1` bypasses. Both editor launchers also reclaim the
  bridge port up front (precise port-owner tree-kill — covers another project's
  live editor, not just zombie `-Cmd` processes). Doctrine: TESTING.md §7.
- `editor_build_game_target`: the Unix-shaped-`project_root` discard (container
  callers passing `/workspace`) now applies only when the build host is Windows —
  on a POSIX host `/...` is a native path. Fixes the four guard-slice test failures
  on Linux CI, where the tests' own `/tmp` scratch roots were being discarded.
- Bun-side coverage ledger for server-local tools (`src/server/test/coverage.test.ts`
  + `test/harness/coverage.ts`): the analog of pytest's `test_zz_coverage.py` for the
  13 registry tools whose wire name has no C++ dispatch key (`catalog_*`, `code_*`,
  `result_read`, `build_status`, `pie_analyze`, `video_analyze`, `editor_read_logs`,
  `editor_build_game_target`, `actor_spawn_physics`) — invisible to the pytest
  manifest by construction. Tests declare coverage with `covers("tool")`; the gate is
  expected-red until docs/loops/tests/TASKS.md §C completes. Supporting change:
  `bridgeTool` now records its wire command on `ToolDef.command` so bridge-tier vs
  server-local classification is runtime-introspectable (legacy overrides like
  `statetree_node_add` → `st_add_node` stay pytest's job).
- `asset_textures_import`: per-image `settings.lod_group` (World/UI/Effects/Skybox/
  Character/Weapon/Vehicle/Pixels2D) and `settings.mip_gen` (FromTextureGroup/
  NoMipmaps/SimpleAverage/LeaveExistingMips). UI-group textures are never streamed,
  so HUD art no longer renders a blurry low mip on first open — previously fixable
  only via the `py` console hatch. Result entries report the applied values.
- `/gimp-import` rewritten around two explicit modes. MOCKUP (the old behavior,
  condensed): one-shot tight-crop + anchor-math recreate of a GIMP layout. CONTRACT
  (new, genericized from a production region-overlay world-map pipeline): a
  layer-naming grammar (`<SCOPE>_<MID>_<STATE>[_<NONCE>]`, structurally parsed),
  co-registered full-canvas exports (zero UMG placement math), filename-as-join-key
  re-import, a committed per-project export script (in-session or headless
  `gimp-console-3`; ad-hoc exports rejected) emitting a validating `manifest.json`,
  and optional CPU hit-mask (`WMSK`) + bounding-box sidecars for irregular-shape
  interaction. New template: `.claude/skills/gimp-import/scripts/gimp_export_contract.py`.
- `physics_material_create`: typed creation of `UPhysicalMaterial` assets
  (friction / static friction / restitution / density; optional per-material
  combine modes that also flip their override flags). Closes the gap where the
  `py` console hatch was the only route to a physical material. PIE-blocked,
  no dry-run, mirrored in `bridge/gates.ts`.
- Server manifest hoisted to the repo root: `package.json` + `bun.lock` now live
  at the top level, so `bun install` / `bun run mcp` / `bun test src/server` work
  straight from a fresh clone (source stays under `src/server/`;
  `src/server/package.json` is gone). `scripts/run-server.*` and the pytest
  harness launch from the root.
- `/onboard` gained a dependency phase: a tiered matrix
  (`.claude/skills/onboard/DEPENDENCIES.md`, minimal vs full vs custom) with
  detection + official install sources per platform; the GIMP pipeline's external
  MCP server is now documented as [maorcc/gimp-mcp](https://github.com/maorcc/gimp-mcp).
- `pie_record_arm`/`pie_record_disarm`: armed auto-record — every PIE session
  records itself from its first frames until disarmed (auto-numbered takes,
  per-take cap). `pie_record_start` gains `wait_for_pie_s` (arm-and-wait for a
  single take).
- `pie_record_start` gains `audio` (default true): recordings carry the PIE
  world's game audio (main-submix mix) as a synced AAC track; result reports
  `audio`/`audio_sample_rate`/`audio_note`.
- PIE video capture + analysis: `pie_record_start`/`pie_record_stop`/
  `pie_record_status` (lease-aware wrappers over the new in-engine recorder),
  `video_analyze` (MP4 + expected behaviour → structured, timestamped verdict
  via a Gemini video-understanding model; key from `GEMINI_API_KEY` or
  `GOOGLE_STUDIO_API_KEY`), and the `pie_analyze` one-shot composite
  (record → stop → analyze). New `video` domain; analysis provider quarantined
  behind `src/video/analyzer.ts`. Config knobs: `UNREAL_MCP_VIDEO_MODEL`,
  `UNREAL_MCP_VIDEO_ANALYSIS_FPS`, `UNREAL_MCP_VIDEO_MAX_ANALYSIS_FPS`,
  `UNREAL_MCP_VIDEO_PROVIDER`. The repo-root `.env` is now backfilled into the
  server env regardless of launch cwd.
- Initial open-source release. Included the progressive-disclosure / code-mode
  subsystem (predates this changelog): the `catalog_domains` / `catalog_search` /
  `catalog_describe` / `catalog_call` metatools, code mode (`code_api` +
  `code_run`, agent-authored TS in a Bun Worker), result compaction
  (`result_read` + `UNREAL_MCP_MAX_RESULT_BYTES`), and the three
  `UNREAL_MCP_SURFACE` modes (`full`/`compact`/`code`). Server-only — no C++
  counterpart (see `src/server/README.md`, "Token efficiency").
