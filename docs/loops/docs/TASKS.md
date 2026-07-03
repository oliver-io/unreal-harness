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

---

## src/server/README.md

---

## CLAUDE.md (repo root)

---

## Skills

(All other 19 skills audited clean: tool names, bundled files, cross-references, and
CLAUDE.md's skill list all verified accurate.)

---

## Changelogs

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

- ~~Coverage manifest is dead~~ — RESOLVED code-side 2026-07-02 by a concurrent agent:
  `regen_operations.py` now derives the manifest from the live Bun registry ∩ C++ dispatch
  keys; `operations.py` regenerated (281 ops). DOC-040 documented the new reality.
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
