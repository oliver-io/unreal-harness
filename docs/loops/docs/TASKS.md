# Documentation-update tasks

Pending work for the [docs update loop](./docs-update-loop.md). Each task is self-contained:
claim → code reality (with evidence) → fix direction. **Code is ground truth** — every item
below was verified against the source by a research pass on 2026-07-02; re-verify the cited
evidence before editing (the code may have moved again). One task per loop iteration.

Conventions: `USAGE` = `docs/USAGE.md`, `ARCH` = `docs/ARCHITECTURE.md`, etc. Evidence is
`file:line` as of the audit. Tasks are grouped by document, ordered most-severe-first within
each group.

---

## docs/BUGS.md

### DOC-002 — Loop docs link a nonexistent `docs/loops/bugs.md`
`docs/loops/mcp/mcp-bugfix-loop.md:3` and `docs/loops/skills/PROPOSALS.md:3` link
`[bugs](../bugs.md)` → resolves to `docs/loops/bugs.md`, which does not exist. The intended
target is `docs/BUGS.md`.
**Fix:** Repoint both links to `../../BUGS.md` (after DOC-001 gives them something to point at).

---

## docs/USAGE.md

### DOC-010 — §2.8 (IK retargeting) is grossly stale: "one primitive" vs 11 shipped tools
USAGE:322-363 claims the MCP exposes "one low-level primitive today"
(`ik_retarget_run_batch`), that "IK Rig + Retargeter asset authoring is intentionally not
exposed," and lists `anim_auto_retarget` as *planned*. Reality: `src/server/src/domains/ik_retarget.ts`
ships 11 tools — `ik_retarget_create` (:33), `_set_rigs` (:62), `_auto_map_chains` (:93),
`_set_chain_mapping`, `_align_bones`, `_set_pelvis_settings`, `_set_root_motion_settings`,
`_import_pose_from_animation`, `_import_pose_from_pose_asset`, `_read` — plus
`ik_rig_list_chains` (`ik_rig.ts:13`). The "planned" auto-mapper is shipped.
**Fix:** Rewrite §2.8 around the shipped authoring surface (contracts + foot-guns), drop the
"intentionally not exposed" design note or scope it to what genuinely remains unexposed
(manual IK-rig authoring — see the design note the code carries).

### DOC-011 — Nonexistent tool names cited as contracts
Four USAGE references name tools that do not exist in any domain module or alias map:
- `physics_spawn_blueprint_actor` (USAGE:227) → real tool is `actor_spawn_physics` (`actor.ts:219`)
- `compile_blueprint` (USAGE:130, 373, 401 — including "required before bind_handler!") → real tool is `bp_compile` (`bp.ts:108`)
- `analyze_blueprint_graph` (USAGE:165, listed as alias of `bp_inspect`) → no such alias exists
- `content_browser_refresh` (USAGE:539) → real name `editor_content_browser_refresh` (`editor.ts:34`)
**Fix:** Rename/remove each. While in §2.4, also add the missing actor tools it omits:
`actor_spawn_physics`, `actor_set_property` (`actor.ts:178`), `actor_get_in_level` (`actor.ts:15`).

### DOC-012 — §2.12 StateTree: "st_* retired" is wrong, and `st_set_entry_state` doesn't exist
USAGE:437 claims "the abbreviated `st_*` aliases were retired in the naming migration" —
but the *wire commands and C++ handler keys are still `st_*`*, mapped via `command:`
overrides (`statetree.ts:146` `command: "st_add_state"`, ~20 overrides total; stated
explicitly in `src/server/src/bridge/gates.ts:12-17`). Only the tool names are
`statetree_*`. Separately, USAGE:448,453 documents `st_set_entry_state` / a
`set_entry_state` step — no entry-state tool exists at all (grep `entry` in `statetree.ts`
= 0).
**Fix:** Reword to "tool names are `statetree_*`; wire/handler keys remain `st_*` via
`command:` override." Remove the entry-state row/step, or document the real mechanism after
verifying how entry states are actually set (likely `statetree_state_set_properties` /
transitions — verify in C++ before writing).

### DOC-013 — "No alias or translation step" (§1.5 / §3.8 / §4) is false
Same root cause as DOC-012 and ARCH DOC-030: a bounded tool-name→wire-command translation
layer exists (`statetree_* → st_*` family, `bp_add_node → add_blueprint_node` at
`bp.ts:532`), plus a parameter-alias normalizer (`src/server/src/registry/aliases.ts`).
**Fix:** Everywhere USAGE states the absolute ("wire name == tool name == handler key, no
alias or translation step"), qualify it: the identity holds *except* for an enumerated,
test-enforced `command:` override set and a bounded parameter-alias map. Keep the spirit
(no open-ended alias layer) without the false absolute.

### DOC-014 — §1.2 error taxonomy: count wrong, `timeout` missing, `isRecoverable()` fictional
- USAGE:33 says "closed set, 30 codes" — both sides define **31** (`errors.ts:12-53`,
  `MCPCommonUtils.h:31-97`); the table (USAGE:35-42) omits `timeout` (added post-doc,
  GAP-007).
- USAGE:53 documents an `isRecoverable(code)` helper "in `errors.ts`" — no such function
  exists anywhere in `src/server` (errors.ts exports only `ErrorCode` and `asErrorCode`).
**Fix:** Add `timeout`, say 31 (or drop the hardcoded count), delete the `isRecoverable`
paragraph (or replace with whatever recoverability guidance is actually true — as of the
audit, nothing implements it).

### DOC-015 — §1.2/§3.2 cite a wrong C++ header path
USAGE:44 and USAGE:645 say `Plugins/UnrealMCP/Source/UnrealMCP/Public/Commands/MCPCommonUtils.h`.
Actual: `src/Plugin/UnrealMCP/Source/UnrealMCP/Public/Commands/MCPCommonUtils.h` (USAGE:3
itself uses the correct form).
**Fix:** `Plugins/` → `src/Plugin/` in both places.

### DOC-016 — §1.4/§3.3 dry-run blocklist massively understated
USAGE:69 and USAGE:659 say "Initial registry: `add_node` / `bp_add_node` (still blocked)".
Reality: `DRY_RUN_UNSUPPORTED` (`gates.ts:99-118`) has ~40 entries (all `ik_retarget_*`,
`gas_*`, `level_*`, factory creators, `pie_record_*`, widget tools, …), and `add_node` is
not a tool name.
**Fix:** Stop implying the set is one tool; describe its categories and point at
`gates.ts` as the authoritative list rather than enumerating (it drifts).

### DOC-017 — §2.18 PIE lease: `pie_not_holder` undocumented; lease codes are out-of-taxonomy
`pie.ts:40-42` defines three server-synthesized codes: `pie_busy`, `pie_lease_lost`,
`pie_not_holder`. §2.18 documents the first two but never `pie_not_holder` (returned by
`pie_stop` when the caller never held the lease — directly relevant to USAGE:600's "only
the holder may stop" claim). All three are also outside the closed error-code set §1.2
promises.
**Fix:** Document `pie_not_holder` in §2.18, and add a note (§1.2 or §2.18) that the three
lease codes are deliberate out-of-taxonomy, server-side codes.

### DOC-018 — New domains with zero USAGE coverage: `pcg_*`, `kinematics_*`, `landscape_*`, `foliage_*`
None of these prefixes appear anywhere in USAGE, including the §1.5 "prefixes in active
use" list (USAGE:88):
- `pcg_*` — 9-10 tools (`pcg.ts`): graph create/read, node add/connect/set_property,
  component add/generate, discovery. **Foot-gun to surface:** the PCG mutators are *not* in
  the C++ PIE/dry-run blocklists, so they silently run during PIE (module header flags this).
- `kinematics_*` — 3 tools (`kinematics.ts`): `_read_transform`, `_probe`, `_solve`;
  editor-world FK probe / two-bone-IK solve reusing game `BoneIK` math; the `/position`
  skill's ground-truth verifier. Document `mode='dryrun'|'live'` and the three spaces.
- `landscape_*` — 3 read-only tools (`landscape.ts`); `foliage_*` — `foliage_inspect`
  (`foliage.ts:14`). Both are inspection-only *by design* (mutation refused — brush-driven
  authoring stays in editor modes).
**Fix:** Add a §2.x per domain (contracts + foot-guns) and extend the §1.5 prefix list.

### DOC-019 — PIE recording + video analysis pipeline entirely undocumented
Zero USAGE coverage for: `pie_record_start/arm/disarm/stop/status` (`pie.ts:578-767`; C++
`MCPRecorderCommands.cpp`/`MCPVideoRecorder.cpp`), `video_analyze` (`video.ts:103`,
`src/server/src/video/analyzer.ts`), `pie_analyze` (`pie.ts:797`), `pie_capture_from_pose`
(`pie.ts:486`), `pie_query` (`pie.ts:330`), `pie_inject_input_action` (`pie.ts:875`). The
§2.18 table (USAGE:568-573) lists only 5 of ~14 PIE tools.
**Fix:** Extend §2.18 (or add §2.19) covering: record lifecycle + `max_duration_s`
watchdog + lease-awareness + refused-under-`-nullrhi`/Windows-only foot-guns; `video_analyze`
contract incl. the server-side API-key requirement (`UNREAL_MCP_VIDEO_*` knobs,
`config.ts:121-125`); `pie_capture_from_pose` as the sanctioned reproducible in-game
screenshot; the remaining PIE tools.

### DOC-020 — Disclosure meta-tools (`catalog_*`, `result_read`) absent from USAGE
`catalog_domains/search/describe/call` (`disclosure/metatools.ts:28/43/67/103`) and
`result_read` (`compaction/tool.ts:15`), both registered (`register.ts:100,102`), have zero
USAGE coverage. They ARE covered in `src/server/README.md` — decide whether USAGE should
carry the agent-facing contract (it claims to hold "every tool contract") or explicitly
delegate to the server README.
**Fix:** Add a short section or an explicit pointer; don't leave them invisible to a USAGE
reader.

### DOC-021 — Partial-coverage domains: documented sections omit shipped tools
- **anim (§2.7):** clip-processing suite undocumented — `anim_smooth_sequence` (:317),
  `anim_normalize_z_offset` (:362), `anim_anchor_feet_to_floor` (:392),
  `anim_extract_between_notifies` (:735) in `anim.ts`. **Data-loss foot-gun:** the first
  three have no dry_run and default to writing a suffixed copy, but an empty
  `output_suffix` mutates the source in place.
- **mesh (§2.4/§2.6):** missing `mesh_set_collision`, `mesh_get_collision`,
  `mesh_get_bounds`, `mesh_build_bend_chain` (heavyweight procedural re-skinner —
  dry_run-able, PIE-blocked, saves mesh+skeleton), socket CRUD (`mesh.ts:141-404`).
- **niagara (§2.14):** omits the creation/renderer set (`niagara_system_create`,
  `niagara_emitter_add(_renderer)`, `niagara_module_add`, `niagara_renderer_set_*`,
  `niagara_mesh_renderer_set_mesh`, `niagara.ts:314-616`) while USAGE:491 still claims graph
  editing "isn't yet exposed".
- **editor (§2.17):** omits `editor_viewport_get_camera` (`editor.ts:129`),
  `editor_build_reflection_captures` (`editor.ts:158`).
- **level (§2.4):** omits `level_new/save/save_as/load/set_gamemode_override` (`level.ts`).
- **gas (§2.10):** omits `gas_ability_set_cost`/`gas_ability_set_cooldown` (`gas.ts:68/97`).
- **physics (§2.16):** omits `physics_set_constraint_motion` (`physics.ts:122`).
**Fix:** One loop iteration per domain bullet is acceptable; update each section and kill
the stale "isn't yet exposed" claims.

### DOC-022 — §3.6 omits the `/build/heartbeat` endpoint
`POST /build/heartbeat` exists (`build/http.ts:103`); §3.6 (USAGE:696-705) lists only
acquire/release/status. **Fix:** add a line. (Everything else in the build-lock contract —
exit 75, TTL, PID-liveness, fail-open — verified accurate.)

---

## docs/ARCHITECTURE.md

### DOC-030 — §2 "no alias or translation layer" needs the same correction as USAGE
ARCH lines 56-59 make the absolute claim; see DOC-013 evidence (`command:` overrides in
`statetree.ts`/`bp.ts:532`, `gates.ts:15-18`, param aliases in `registry/aliases.ts`, C++
resolver comment `MCPCommonUtils.h:178-181`).
**Fix:** Soften to: wire name == handler key; tool name is canonical agent-facing; a small,
enumerated, test-enforced translation set exists (`gate-error-parity.test.ts`,
`aliases.test.ts`).

### DOC-031 — Tool-count figure conflicts three ways (ARCH ~260, config 233, actual ~285)
ARCH:72 and ARCH:135 say "~260"; `config.ts:58,60` says "233 domain tools" (3×); grep of
tool `name:` declarations ≈ 285 (+ 4 `catalog_*`, `result_read`, `code_api`/`code_run`,
`mcp_status`, `ping`). Same stale "~260" appears in `src/server/README.md:42,45` and
CLAUDE.md ("~260 canonical tools").
**Fix (doc side):** Stop hardcoding counts; reference the boot log's runtime figure
(`[surface=…, N/M tools advertised]`) or use a loose band, consistently across ARCH, server
README, and CLAUDE.md. The stale strings inside `config.ts` are code-side — see #DEFERRED.

### DOC-032 — Boot-gate attribution: pending is server-side, not plugin-side
ARCH attributes all three gates (boot → PIE → dry-run) to "the plugin side," but the
*pend-during-boot* behavior lives in the server (`bridge/connection.ts:12-14`); the C++ side
only refuses with `editor_not_ready` (`MCPCommonUtils.h:86`).
**Fix:** One-line clarification of which side pends vs refuses.

### DOC-033 — Concepts described but concrete surfaces unnamed
ARCH §2 describes the searchable-catalog mode, code-execution mode, and result compaction
but never names `catalog_*`, `code_api`/`code_run`, `result_read`, or the
`UNREAL_MCP_SURFACE=full|compact|code` knob; it mentions the build lock but not that
`scripts/build-coord.ps1` drives it over the `/build` REST endpoints.
**Fix:** A sentence each so a reader can map doc→tool. Keep the doc conceptual; just anchor
the names.

---

## docs/TESTING.md

### DOC-040 — §8 coverage-oracle description is the opposite of the implementation
TESTING §8 (lines 183-188): coverage "fails the suite if any operation **in the registry**
is uncovered … Coverage is not aspirational." Reality: (a) the guard is explicitly designed
to be "EXPECTED to be red until coverage is complete" and prints a completion percentage
(`tests/harness/coverage.py:6`, `tests/integration/test_zz_coverage.py:27-31`) — a
scoreboard, not a gate; (b) it checks against a hand-frozen 248-entry manifest
(`tests/harness/operations.py`), not the live registry, and the regenerator
(`tests/tools/regen_operations.py:16`) scans the dead `src/MCP/` path, so the manifest can't
be regenerated and silently omits newer tools (`video_analyze`, `pie_analyze`,
`pie_capture_from_pose`, `pie_inject_input_action`, `catalog_*`) while retaining the legacy
wire name `add_blueprint_node`.
**Fix (doc side):** Rewrite §8 truthfully: a generated-manifest scoreboard, currently
frozen/stale, expected red. The generator repoint + manifest regen is code-side — see
#DEFERRED. (CLAUDE.md's "fails if any bridge operation lacks a test" line inherits the same
correction.)

### DOC-041 — Video evidence path missing from the doctrine
TESTING Tier-2b (lines 100-106) names only `editor_screenshot`/`editor_window_screenshot`
as render evidence. The infra has a full second channel: `pie_record_*` → `.mp4` →
`video_analyze` (exercised end-to-end by `tests/integration/test_pie.py:160-232`).
**Fix:** Add a Tier-2b subsection documenting video capture + vision-oracle analysis as
render evidence, with the same "state the expectation up front" discipline.

### DOC-042 — Capture-rig requirement referenced by the skill, absent from the doctrine
`.claude/skills/capture-pose/SKILL.md` says it builds "the fixed capture rig that …
docs/TESTING.md require[s]" — TESTING.md never mentions capture rigs or
`pie_capture_from_pose` (the requirement lives only in CLAUDE.md's VERBOTEN section).
**Fix:** Add the fixed-capture-rig rule + `pie_capture_from_pose` to Tier-2b so doctrine,
CLAUDE.md, and the skill agree.

---

## docs/DEBUGGING.md

### DOC-050 — Wrong tool names: `read_logs` / `build_game_target`
Lines 96, 97, 99, 108 cite bare names; canonical tools are `editor_read_logs`
(`editor.ts:404`) and `editor_build_game_target` (`editor.ts:219`); no alias maps the bare
forms. **Fix:** prefix throughout §4/§5.

### DOC-051 — Bogus sanity check `bun run mcp --help`
Lines 39-41. `main.ts` parses no argv — the command boots a real server on :8765 (then the
next real start trips the already-listening guard). **Fix:** replace with a real probe
(`bun run typecheck`, or the §3 `ping`/`mcp_status` smoke).

### DOC-052 — Missing troubleshooting topics (verified failure modes with no doc entry)
Add a row/section each for:
1. **Duplicate editor on 55557** — second editor's bind fails silently
   (`MCPBridge.cpp:416-420`, only a log-file error); symptom: commands land on the wrong
   editor. Fix: close the duplicate; grep editor log for "Failed to bind listener socket".
2. **Build lock / exit 75** — build scripts refuse with exit 75 while a build holds the
   lock (`src/server/scripts/verify-build-lock.ts:138,146`); wait and poll `/build/status`.
3. **Stale/orphaned server on 8765** — `run-server.sh:27-38` refuses to start; remedy
   `scripts/stop-server.sh` then re-run (doc currently frames 8765-held as "unrelated app").
4. **`video_analyze` missing API key** — pure server-side provider call; failure mode and
   the `scripts/google-key.sh`/`scripts/openai-key.sh` helpers + `UNREAL_MCP_VIDEO_*` knobs
   (`config.ts:121-125`) are undocumented.

---

## src/server/README.md

### DOC-060 — Stale counts and incomplete architecture tree
- Line 82: "29 + core" domains → actually 34 + core (35 modules excl. `_schemas.ts`/`_shared.ts`).
- Lines 42, 45: "~260" tools → ~290 (see DOC-031; use the same counting policy chosen there).
- Lines 69-85: tree omits three load-bearing dirs — `build/` (REST build lock, `http.ts`/`lock.ts`),
  `pie/` (`lease.ts`/`reconciler.ts`, wired in `main.ts:17-18`), `video/` (`analyzer.ts`);
  and `registry/` row omits `aliases.ts`.
- Env table (lines 32-38) missing the five `UNREAL_MCP_VIDEO_*` knobs (`config.ts:121-125`).
**Fix:** one pass updating all four.

---

## CLAUDE.md (repo root)

### DOC-070 — VERBOTEN/capture-rig section never names the sanctioned tool
The "fixed capture rig" rule names only the `/capture-pose` skill; the underlying sanctioned
tool `pie_capture_from_pose` (`pie.ts:486`) — whose own description calls it "the sanctioned
way to get a reproducible in-game screenshot" — is never mentioned in the doctrine that
demands it. **Fix:** name the tool alongside the skill. Also fold in the DOC-031 count fix
("~260 canonical tools") and, optionally, add `install-plugins`, `google-key.sh`,
`openai-key.sh` to the `scripts/` layout line (currently omitted).

---

## Skills

### DOC-080 — /onboard: wrong hook filename + un-prefixed tool names
- SKILL.md Part 3 §11 (~line 320) cites `.claude/hooks/openai-cred-gate.sh` — the file is
  `.claude/hooks/openai-cred-gate.py` (`/icon` SKILL.md:60 has it right).
- Part 1 (~110) and Part 2 (~205) cite `build_game_target` / `read_logs` — canonical names
  are `editor_build_game_target` / `editor_read_logs` (`/bootstrap` SKILL.md:295 has it right).
**Fix:** both renames.

### DOC-081 — /automated-tester: two misleading operational details
- §2 line 74 cites `uassetDiskPath` as if it were a shared harness helper; it is a
  file-local function in `src/server/test/integration/asset.test.ts:32`, not exported from
  `test/harness/ops.ts`. Doc-side fix: say "copy the local helper from `asset.test.ts`"
  (promoting it into the harness is code-side — see #DEFERRED).
- §6 lines 170-183: fenced block opens with `cd src/server && bun test` then lists
  `scripts/launch-editor.ps1` / `scripts/run-server.ps1`, which live at repo root — run
  verbatim the paths resolve wrong. Fix: qualify as repo-root paths.

(All other 19 skills audited clean: tool names, bundled files, cross-references, and
CLAUDE.md's skill list all verified accurate.)

---

## Changelogs

### DOC-090 — Disclosure/code-mode subsystem absent from both changelogs
`catalog_*` / progressive disclosure / `code_run` appear in neither `CHANGELOG.md` nor
`src/Plugin/UnrealMCP/CHANGELOG.md` despite being a central server feature (README "Token
efficiency"). Likely predates the "Initial open-source release" entry — if so, add it under
that entry (or a "pre-release" note) so the "authoritative history" claim holds. Low
priority; everything recent (pie_record_*, video_analyze, physics_material_create, texture
LOD) is properly logged in both.

---

# UNVERIFIED — suspicions needing confirmation before acting

Do **not** edit docs from these without first verifying; if verified, promote to a task.

- DEBUGGING §3 "pends calls up to ~120 s" and the `{ready, phase, pie_active}` `mcp_status`
  shape — timeout constant and result struct not confirmed.
- USAGE §2.17 `/build/status` field shape (`holder:{label,pid,target,held_ms,expires_in_ms,pid_alive}`)
  — route confirmed, exact field names in `build/lock.ts` not opened.
- USAGE §1.2 table grouping (`ambiguous_target`, `unsupported_class`) may not mirror the
  enum's grouping — cosmetic.
- ARCH claim of fixed gate *ordering* (boot → PIE → dry-run) in C++ dispatch — all three
  gates confirmed to exist; sequence in `FMCPBridge::ExecuteCommand` not traced.
- TESTING §6 "passing dry-run implies passing commit" parity invariant — behavioral claim
  not audited.
- Whether each pytest `@covers` truly has a Bun mirror (§8/§9) — filenames line up 1:1;
  case-by-case not verified.
- server README "~80-120k tokens if loaded up front" — not measured.
- Advisor reference files' internal §-numbers (gamelift `QUEUES.md`/`PLATFORM.md`/
  `PATTERNS.md`, networking `REPLICATION.md`/`AUTHORITY.md`) cited by their SKILL.md bodies
  — files exist, internal section numbers not opened.

---

# DEFERRED

Items understood but **out of this loop's lane** (they require code changes, not doc edits).
Route to the MCP bugfix loop (`docs/loops/mcp/TASKS.md`) or a human decision:

- **Coverage manifest is dead:** `tests/tools/regen_operations.py:16` scans the removed
  `src/MCP/` tree; `tests/harness/operations.py` is frozen at 248 pre-migration ops (missing
  `video_analyze`, `pie_*` additions, `catalog_*`; still contains legacy `add_blueprint_node`).
  Needs the generator repointed at the TS registry or C++ dispatch, then a regen. (Doc task
  DOC-040 only fixes the *description*.)
- **`config.ts:58,60` "233 domain tools"** — stale count strings inside code comments/log
  labels; reconcile when fixing DOC-031's counting policy.
- **Stale code comments referencing the deleted Python server** — `errors.ts:9`,
  `envelope.ts:13` ("Port of `src/MCP/helpers/error_codes.py`"), `tests/conftest.py:115,147`
  ("the Python MCP server"), `ik_retarget.ts:5` etc. Misleads readers into thinking a Python
  parity source still exists.
- **PCG mutators missing from PIE/dry-run blocklists** (C++ + `gates.ts`) — currently run
  unguarded during PIE; blocklist addition is a both-sides code change per USAGE §3. DOC-018
  documents the hazard meanwhile.
- **PIE lease codes outside the closed error taxonomy** (`pie_busy`/`pie_lease_lost`/
  `pie_not_holder`, `pie.ts:40-42`) — either adopt into `EMCPErrorCode`+`errors.ts` or bless
  as a documented exception (DOC-017 documents the status quo).
- **Promote `uassetDiskPath`** from `asset.test.ts:32` into `test/harness/ops.ts` (pairs
  with DOC-081).
