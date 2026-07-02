---
name: refactor
description: Run a disciplined, document-driven refactoring loop on a game (or on the harness's own Bun/TS code) — DRY/functional C++ extraction, TypeScript service de-duplication, UMG/Slate widget consolidation, or feature-first project/asset structure. Genericized from a production UE reference project's proven refactor loops. One namespace, four modes. The loop is anchored to a worklist document with three areas — TODO / DONE / DEFERRED — and processes exactly ONE logically distinct refactor per iteration under a strict architectural-purity + scope contract (zero behavior change, no new abstractions, locality of behavior, bias toward leaving things alone). Use for "refactor this", "DRY up X", "de-duplicate the widgets", "extract the math/logic", "split a 600-line file", "clean up the service layer", "restructure the Content tree", "find extractable utilities". Behavior-preserving only — it never adds features. Complements `/architect` (writes the design) and `/docs` (keeps the taxonomy synced).
user-invocable: true
allowed-tools: [Read, Write, Edit, Grep, Glob, Bash, Skill, AskUserQuestion, mcp__unrealMCP__asset_list, mcp__unrealMCP__asset_references, mcp__unrealMCP__asset_move, mcp__unrealMCP__asset_rename, mcp__unrealMCP__asset_duplicate, mcp__unrealMCP__asset_fixup_redirectors, mcp__unrealMCP__project_context, mcp__unrealMCP__class_query, mcp__unrealMCP__class_inspect, mcp__unrealMCP__catalog_search, mcp__unrealMCP__widget_tree_read, mcp__unrealMCP__editor_live_coding_compile, mcp__unrealMCP__build_status]
---

# refactor — the document-driven refactoring loop

A repeatable, **net-zero-behavior** improvement loop. You find one clean unit of duplication or misplacement, prove it is real, extract it to the nearest sensible home, migrate every call site, verify nothing changed, and record it — then repeat until no eligible candidate remains. The discipline is the point: **the default is to leave code alone**, and the loop exists to make the *rare, unambiguous* win safe and reviewable, not to manufacture churn.

Genericized from a production UE reference project's refactor loops (`refactor_general` / `refactor_typescript` / `refactor-ui` / `refactor-structure`). It hardcodes none of that project's paths or classes — it derives roots from the project you point it at.

## Where it runs

Refactoring is **per-target**. Pick the target from the request:
- A **game** under `projects/<Name>/` — its `Source/` (C++) and `Content/` (assets).
- The **harness's own Bun/TS** — `src/server/`, `tools/`, `scripts/`, or a game's `service/` / `web/`.

The worklist document lives **next to what it tracks**, under that target's `docs/loops/`:
`<target>/docs/loops/refactor-<mode>.md` (e.g. `projects/hoverball/docs/loops/refactor-functional.md`, `src/server/docs/loops/refactor-ts.md`). Create it on first use from the template below. **It is the loop's memory** — read it first every run; never start a pass without it.

## Modes (the family)

Invoke `/refactor <mode>` (default `functional`). Each mode shares the **same loop engine and the same purity law** (below); they differ only in *what counts as a candidate* and *how you verify*.

| Mode | Target | Hunts for |
|------|--------|-----------|
| `functional` (default) | C++ game source | Pure logic (math/physics/queries/predicates) embedded in a class that should not own it; re-implemented DRY violations → free functions in a **local** namespace |
| `ts` | Bun/TS (server, service, web, tools) | Logic duplicated across services, cross-service coupling, type drift, route/handler shape, logger normalization, test placement |
| `ui` | UMG/Slate widgets | Identical widget-construction sequences (backgrounds, dividers, buttons, rows, modals) → a shared component |
| `structure` | `Content/` + project layout | Feature-first violations: vendor-path references, type-first organization, gibberish names, files in the wrong folder, 600-LOC files |

---

## THE LAW (every mode, no exceptions)

These are the non-negotiables. If a candidate violates any of them, it is not a candidate.

1. **Zero behavior change.** Same public API, same execution order, same side effects, same float types/operations. For `ts`: same HTTP status, JSON shape, DB writes, event emissions, and log field names — a change a `curl` probe or a log consumer can see is a *feature*, not a refactor. If a refactor reveals a behavior worth improving, that is a **separate** change — never bundle it.
2. **Bias toward leaving things alone.** The default is *do not refactor*. Act only when duplication is unambiguous, the benefit is concrete, and the result reads simpler than what it replaces. **If you find yourself arguing for an extraction, it is not worth doing.**
3. **No new abstractions.** No base class, template, trait, interface, virtual, callback, or config struct that does not already exist — *unless removing the duplication strictly requires it and the requirement is obvious*. A helper that needs flags-and-callbacks to paper over real per-site differences is worse than the duplication. Constructors-as-data-sheets and explicit two-line idioms are the readable form; do not "DRY" them.
4. **Locality of behavior.** Do not move an implementation far from its callers. Extract to the **nearest existing** namespace/helper in the same folder; create a new small local one *only* when none fits. Generality is earned by 3+ real consumers across 2+ files, not anticipated.
5. **One logically distinct refactor per iteration.** Independently reviewable and reversible. Never batch unrelated refactors.
6. **When in doubt, leave a comment, not a change.** If a call site *looks* like it should use the shared form but intentionally differs, add a one-line `// Intentionally … — see X` so the next pass does not re-investigate. If you cannot tell whether a difference is intentional, investigate the history or **ask** — never silently force it into the shared pattern.
7. **Do not hunt for weaker candidates.** If the scan turns up nothing that clears the bar, the loop is **complete** — record the deferral and stop. Lowering your own standard to justify doing work is the failure mode this loop is built to prevent.

---

## The worklist document — three areas

The doc *is* the loop. It has exactly three living sections (plus an optional plan/cursor for systematic sweeps):

```markdown
# Refactor loop — <mode> · <target>

Scratch-pad + ledger for the <mode> refactor loop. ONE logically distinct refactor per iteration,
net-zero behavior change. Design intent lives in CLAUDE.md / docs/architecture — this file is the worklist.
Read THE LAW in the /refactor skill before each pass.

## TODO
Candidates found but not yet enacted, and "observations for the architect" (a behavior question or
latent bug you noticed but must NOT fix here — flag it, do not touch it). One bullet each.

## DONE
Completed refactors, newest first. Each entry: what was duplicated, what was extracted, where it
lives now, the call sites migrated, the intentional divergences left at sites, ~LOC removed, and the
verification (build/test result). This is the audit trail — be specific enough that it is reviewable
without re-reading the diff.

## DEFERRED
Investigated and REJECTED, with the one-line reason (so future passes don't re-litigate the same
decision). "REJECTED — <pattern>: <why it's the readable form / why the abstraction is worse>."
```

For a large codebase, prepend a **PLAN**: an ordered list of domain clusters (highest-duplication-risk first) and a **cursor** noting which pass you are on, so the sweep is systematic and resumable across runs.

---

## The loop (one iteration)

Run this once per invocation. It maps to Scan → Investigate → Decide → Execute → Verify → Record.

1. **Read the worklist.** Load `<target>/docs/loops/refactor-<mode>.md` (create from template if absent). Pick up the cursor / `## TODO`. Re-read THE LAW.
2. **Scan for ONE candidate.** Grep the current cluster for the mode's duplication signals (see each mode below). Prefer something already in `## TODO`.
3. **Investigate — prove it is real.** Read *every* implicated site. Never refactor on a name match. Understand *why* the sites differ. Apply the mode's act/pass criteria.
4. **Decide.** If the benefit is marginal, the extraction would need a new abstraction, or you are arguing with yourself: **stop.** Append a `## DEFERRED` line and report that no actionable candidate was found. **Do not loop into weaker work.**
5. **Execute — one unit.** Extract to the nearest existing home (or a new small local one). Migrate *all* appropriate call sites. Leave intentional divergences at their sites with a comment. Add nothing beyond what the extraction needs.
6. **Verify — zero behavior change.**
   - **C++ (`functional`/`ui`):** it must compile. Prefer **Live Coding** (`editor_live_coding_compile`, or `scripts/build-editor.ps1` for reflection changes) — and **respect the harness build-coordination + multi-agent rules in the root `CLAUDE.md`** (don't stop a shared editor or take the build lock without permission; pre-existing errors in files you didn't touch are HANDS-OFF). No business logic added or removed.
   - **`ts`:** `bun test` green (failures must be unrelated + pre-existing), `bun run typecheck`, the server boots, and a representative route returns byte-identical wire format. If the area has no test, **write the test first**, confirm it passes against current behavior, *then* refactor.
   - **`structure`:** references still resolve (run `asset_fixup_redirectors` after a move/rename); the editor still loads the asset; no dangling vendor paths.
7. **Record.** Newest-first entry in `## DONE` with the rationale + verification. Tick the cursor. If the refactor created/renamed/moved/deleted a file, **sync the docs**: invoke `/docs` (or update the project's `TAXONOMY.md`/architecture doc) so the index never drifts.
8. **Loop or stop.** Another obvious candidate in this cluster? Repeat from 2. Otherwise advance the cursor to the next cluster, or — if the whole sweep is dry — declare **COMPLETION** (no eligible candidates remain) and stop.

**Completion is a real, reportable state.** "I found nothing worth doing" is success, not failure.

---

## Mode criteria

The LAW is shared; these are the per-mode act/pass tests. Read the matching reference-project source loop for worked examples if you want depth — but the criteria below are the contract.

### `functional` — DRY/functional C++ extraction
Hunt: pure logic a class shouldn't own (a bow computing projectile intermediates; an ellipse area inside grenade code; a repeated game-state query; copy-pasted collision/equip logic; a chain re-implemented across siblings).
- **Act when ALL:** 3+ near-identical sites across 2+ files · the logic is a self-contained unit with clear in/out · the extracted function has fewer parameters than the replaced code has lines · >~10 LOC saved overall **OR** the duplication is a correctness risk (two paths that must stay in sync). Prefer **free functions in a namespace** over members; result structs (`F…Result`) for multi-value returns; `TOptional<T>` over bool out-params.
- **Pass when ANY:** <3 sites · sites differ meaningfully in params/fallback/intent · it's framework boilerplate (GAS apply, `TActorIterator`, `FSlateBrush` setup) · extraction needs a new abstraction. Put a new `<Domain>Math`/`<Domain>Rules` header in the **same folder** only when no existing namespace fits.

### `ts` — Bun/TypeScript service de-duplication
Hunt (in priority order): logic duplicated across services → a `lib/` free function *only if cross-service, else stays in the service*; **cross-service coupling** (`services/X` importing `services/Y` — the biggest structural regression; fix via `lib/`/`types/` or the event bus); type drift between `routes.ts` and `service.ts`/`types.ts`; inline route handlers doing real work; `console.*`/`JSON.stringify` logs that should be structured logger fields (**never rename fields**); `*.test.ts` living under `services/` (move to `tests/` mirroring the path); 600-LOC files (`Module_Concern.ts`, re-export unchanged); inline DDL that belongs in `schema.ts`.
- **Pass when** sites differ in error mapping, audit-log shape, transactional boundary, or log field structure — the things that *look* the same and aren't. Trust the type system but don't weaponize it: only remove `as any`/`@ts-ignore` when you can prove the invariant; else leave it with a one-line comment.

### `ui` — UMG/Slate widget consolidation
Hunt: identical widget-tree construction (backgrounds, dividers, buttons, tab bars, rows, modals) built the same across files.
- **Act when ALL:** 3+ near-identical sequences across 3+ widget files · differences reduce to parameters (color/label/size) without bloating the API · the component has a one-sentence responsibility. Add it to the project's UI folder with a header comment (what it builds, when to use it, when **not** to). Return structs give callers handles to the parts they customize. New shared colors/spacing go in the style header only if used in 2+ widgets or they're a semantic concept — else file-local.
- **Pass when ANY:** instances intentionally differ in style/behavior · a helper saves <3 lines/site · the param list would exceed the inline code · the instances use fundamentally different approaches (`DrawAs` vs `TintColor`).

### `structure` — feature-first project/asset layout
Hunt: assets referenced from a **vendor path** instead of a project copy; **type-first** organization instead of feature-first; vendor-gibberish names instead of descriptive PascalCase with standard prefixes (`SM_`/`SK_`/`M_`/`T_`/…); a file in the wrong folder; a 600-LOC source file to split (`ClassName_Concern.cpp`, header unchanged, UBT auto-discovers).
- **Act when:** a used asset still points at vendor content · organization is type-first · names are gibberish. Duplicate the used asset into `<Project>/{Feature}/{Type}/` with a clean name, repoint references, `asset_fixup_redirectors`.
- **Pass when:** the asset is in a vendor folder and **not yet used** (leave vendor trees untouched) · restructuring would churn many references for no readability gain · the current layout is already clear.

---

## Boundaries (what this skill is NOT)

- **Not a feature loop.** It changes *form*, never *behavior*. A behavior improvement you spot goes to `## TODO` as an "observation for the architect" — flagged, not fixed.
- **Not an architecture writer.** It does not design systems — that's `/architect`. It does not invent new layers; it consolidates within the existing one (granular typed primitives stay primitive; composites stay in the server layer — see `docs/ARCHITECTURE.md`).
- **Not a doc indexer.** It *triggers* a doc sync after a file move, but `/docs` owns the taxonomy.
- **Not a build-stomper.** It obeys the multi-agent build-coordination rules in the root `CLAUDE.md` — shared editors and the build lock are not yours to seize.
