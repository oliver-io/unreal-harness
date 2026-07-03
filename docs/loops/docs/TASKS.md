# Documentation-update tasks

Pending work for the [docs update loop](./docs-update-loop.md). Each task is self-contained:
claim → code reality (with evidence) → fix direction. **Code is ground truth** — every item
below was verified against the source by a research pass on 2026-07-02; re-verify the cited
evidence before editing (the code may have moved again). One task per loop iteration.

Conventions: `USAGE` = `docs/USAGE.md`, `ARCH` = `docs/ARCHITECTURE.md`, etc. Evidence is
`file:line` as of the audit. Tasks are grouped by document, ordered most-severe-first within
each group.

---

## docs/USAGE.md

## docs/ARCHITECTURE.md

---

## docs/TESTING.md

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
