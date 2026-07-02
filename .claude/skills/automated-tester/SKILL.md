---
name: automated-tester
description: Set up an automated, agent-observed end-to-end test (or a guarded dev loop) for an Unreal feature in this harness. Use when someone wants to "add a test", "test this feature end-to-end", "verify X works in PIE", "build a verification harness", "guard this feature with a test", or "make a guardrail loop for an agent building X". Opinionated: the engine constructs the scenario via typed C++ primitives, the agent observes the behaviour it controls through a DIFFERENT read primitive, no mocks, headless-first. Encodes docs/TESTING.md.
user-invocable: true
allowed-tools: [AskUserQuestion, Bash, PowerShell, Read, Write, Edit, Grep, Glob, Skill, Agent, mcp__unrealMCP__*]
---

# automated-tester

Stand up a deterministic, headless-runnable test that proves an Unreal feature works
— and, when asked, wire it into a **guarded dev loop** so it gates an agent building
that feature. This skill applies the doctrine in `docs/TESTING.md`; read that file
once if you haven't — this SKILL is the procedure, that doc is the *why*.

**The opinion, in one line:** a feature isn't done until a test **arranges** its
preconditions through typed primitives, **acts**, and **asserts on observed state
read back through a different primitive** — and if you can't arrange or observe it
through a typed command, you **add that command to the C++ plugin**, you don't weaken
the test. No mocks. No native UE automation. The MCP surface *is* the test API.

> Before anything else: never mock the engine, never reach for `FAutomationTestBase`/
> Gauntlet, never assert only on a mutator's own echo. If those feel necessary, the
> arrange/observe surface has a gap — fill it in C++ (§4).

---

## 0. Frame the feature (one short interaction)

Establish four things before writing a line of test. Infer what you can from the
codebase; ask the user only what you genuinely can't.

1. **What is the observable contract?** What *visible state change* proves the
   feature works — an asset on disk, an actor property, a moving pawn, a score, a log
   line? "It compiled" is not a contract.
2. **What typed command ARRANGES the precondition?** (`actor_spawn`, `level_load`,
   `bp_create_blueprint`, `pie_start`, …)
3. **What typed command OBSERVES the outcome** — and is it *different* from the one
   that caused it? (mutate with `actor_set_transform`, observe with `actor_inspect`.)
4. **Which tier?** (§2) Pick the **lowest** tier that can prove it.

If arrange or observe has no typed command, that's the real first task — go to §4.

---

## 1. Pick the tier (lowest that proves the behaviour)

| Tier | When | Runs under | Observe via |
|---|---|---|---|
| **1 — Static / asset** | asset CRUD, blueprint/material/statetree authoring, editor-world actor state | headless `-nullrhi` | `*_inspect` / `*_read` + on-disk `.uasset` |
| **2 — Runtime / PIE** | gameplay, physics, AI, anything that only manifests while playing | headless for queries; **GUI** if it opens a viewport | structured **log markers** (primary), `pie_query`, `ai_get_*`, `editor_perf_snapshot` |
| **2b — Pixel / render** | "does it look right" | **GUI only** | `editor_screenshot` + `/see` / vision; assert on `bytes`/path, not pixel-diff |

Most features are Tier 1. Reserve Tier 2 for genuine runtime behaviour; reserve 2b
for when a human/vision check of pixels is the *only* proof.

---

## 2. Author the test (Arrange → Act → Observe → Assert)

**Default to a Bun in-process integration test** in `src/server/test/integration/<domain>.test.ts`
— fast, no socket, no leaks, auto-skips when no editor is up. Copy
`templates/integration.test.ts` from this skill as the skeleton, or model on a real
neighbour (`src/server/test/integration/actor.test.ts`, `blueprint.test.ts`,
`statetree.test.ts`).

The four beats, each non-negotiable:

- **Arrange** — idempotent (`ensureAbsent` before any create) and namespaced (`NS` =
  `/Game/__MCPTest__`). Put shared setup in `beforeAll`. Use harness helpers from
  `test/harness/ops.ts` (`ensureAbsent`, `payload`, `isFalsyOrEmpty`, `assertReady`,
  `firstAssetOf`).
- **Act** — the one operation under test. For mutators, consider a `dry_run: true`
  pre-flight assertion too (§3).
- **Observe** — read back through a **different, deeper** primitive. For asset
  creation, the strongest observe is **the `.uasset` exists on disk** (`uassetDiskPath`),
  not that the create returned success.
- **Assert** — compare observed state to expectation; end risky tests with
  `await assertReady(ctx.mcp)` (crash guard).

```ts
editorSuite("myfeature", (ctx) => {
  beforeAll(async () => {
    await ensureAbsent(ctx.mcp, `${NS}/BP_Sample`);
    await ctx.mcp.expect("bp_create_blueprint", { name: `${NS}/BP_Sample`, parent_class: "Actor" });
  });
  test("does the observable thing", async () => {
    await ctx.mcp.expect("<mutator>", { /* ... */ });            // ACT
    const got = await ctx.mcp.expect("<different reader>", { /* ... */ }); // OBSERVE
    expect(/* field of got */).toBe(/* expected */);             // ASSERT
    await assertReady(ctx.mcp);
  });
});
```

**Mirror it as a `@covers` pytest test** in `tests/integration/test_<domain>.py` so
the coverage oracle stays green (§5). Same AAA(O) shape; `mcp.expect(tool, {...})`.

### Tier-2 (PIE) specifics

- **Poll, never sleep.** PIE start/stop is async — `pie_start` returns `starting`;
  poll `pie_get_state` to a deadline until `is_running` flips (see `waitForPie` in
  `pie.test.ts`). Same for stop.
- **Gate viewport tests.** Tests that open a game window are GUI-only:
  `test.skipIf(!GUI)(...)` (Bun) / `@pytest.mark.gui_only` (pytest).
- **Always tear down PIE.** `afterEach`/`finally` that force-`pie_stop` (harmless
  `not_in_pie` no-op if nothing's playing). Never leave a session running.
- **Assert on log markers first.** Instrument the feature to emit a stable prefix
  (`[FEATURE] event=... field=...`); assert via `editor_read_logs`. This is the
  primary runtime oracle — deterministic where screenshots and timings are noisy.
- **Prefer self-driving subjects.** A bot / auto-tick BP / placed director actor you
  *observe* beats synthetic keystrokes you *inject* — `pie_send_keystrokes` doesn't
  reach polled input (GAP-030), and GameMode `BeginPlay` doesn't fire in PIE
  (GAP-031). Drive from a placed `BP_*Director`, verify via `pie_query` + markers.

---

## 3. Dry-run pre-flight (for mutators)

When the feature is a mutator, add a dry-run test: call with `dry_run: true`, assert
`result.dry_run === true` and inspect `result.diff` — then assert **no side effect
happened** (read back, confirm unchanged). Validation parity means a green dry-run
implies a green commit (`docs/USAGE.md` §1.4). If the mutator returns
`dry_run_unsupported`, that's expected for append-only/creation ops — assert that
error code instead (it's the documented "not ready for dry-run" signal).

---

## 4. When arrange or observe is missing — extend the plugin

This is the heart of the doctrine. If you cannot construct the scenario or read the
outcome through a typed command, **add the command to the C++ plugin** rather than
scripting around it. The scenario lives in the engine, typed and replayable.

- Write a `HandleCommand`-style handler in the right domain `.cpp`
  (`src/Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands/...`); parse with
  `TryGet*Field`; on bad input return `FMCPCommonUtils::CreateErrorResponse(msg,
  EMCPErrorCode::Code, hint)`; return a plain result object on success.
- Wire the name into `FMCPBridge::ExecuteCommand`'s dispatch chain. If it mutates an
  asset, follow `PreEditChange → mutate → PostEditChange → MarkPackageDirty` (server
  auto-saves). Add to `IsBlockedFromDryRun`/`IsBlockedDuringPie` if applicable and
  mirror into `src/server/src/bridge/gates.ts`.
- Surface it as a tool in `src/server/src/domains/<domain>.ts`, register in
  `src/register.ts`, run `bun run lint:names` + `bun run typecheck`.
- Full contract: `docs/USAGE.md` §3 and `CLAUDE.md` → "Adding/editing a C++ command".

Prefer a **granular, observation-oriented** primitive (a reader/inspector, a precise
mutator) over a convenience composite — composites belong in the server layer, and
some are refused by design (`docs/ARCHITECTURE.md` §5; read it before adding anything
that smells like a recipe). After rebuilding the editor, probe the new command with
`bun src/server/scripts/bridge-call.ts <cmd> '<json>'` before relying on it.

---

## 5. Coverage — keep the parity oracle green

`tests/integration/test_zz_coverage.py` statically scans `@covers("op", ...)`
literals and **fails the suite if any registry operation is untested** (and catches
typo'd op names). So: every new command needs a `@covers` pytest test. Mark your
test, e.g. `@covers("myfeature_do", "myfeature_inspect")`.

> The **Bun suite mirrors but does not yet enforce** coverage. Always add the pytest
> `@covers` test (the gate) in addition to the Bun test (the fast path). If you're
> asked to "make coverage real on the Bun side," that's a known standing improvement
> — a Bun coverage oracle scanning the integration suite.

---

## 6. Run it and read the evidence

```bash
# Fast in-process path (auto-skips editor-gated suites if no editor is up):
cd src/server && bun test
cd src/server && bun test test/integration/myfeature.test.ts   # one file

# Against a live editor (start one first, or let the pytest harness own it):
scripts/launch-editor.ps1            # headless; or add -Gui for render/PIE-viewport tests
scripts/run-server.ps1               # the Bun MCP server

# The parity gate (builds + launches a real editor, runs the coverage oracle):
tests/run.ps1                        # headless
tests/run.ps1 --ue-mode=gui          # GUI (render/PIE-viewport tests)
tests/run.ps1 --ue-attach            # attach to an already-running editor (fast iterate)
```

**Reading the evidence is the point** — don't just check green/red:
- Failure → pull the actual state: `editor_read_logs` for your markers, re-run the
  observe command, inspect the `diff`, capture a screenshot if Tier 2b.
- A test that passes without the feature implemented is a **bad test** (asserting on
  the echo, not observed state) — strengthen the observe step.
- Boot/timeout flake → confirm the client polled `mcp_status` to ready (never a fixed
  sleep), and that exactly one driver is touching the editor.

---

## 7. (Optional) The guarded dev loop

When the ask is "guard an agent building feature X" (not just "add a test"), produce
a **standing verification** the loop runs on every change:

- For **per-game E2E behaviour**, write/extend `src/server/scripts/verify-<game>.ts`
  — a bounded PASS/FAIL script (real MCP client → boot PIE → drive → assert markers +
  `pie_query` positions → screenshot → `pie_stop`), with a **wall-clock watchdog** and
  a `finally` `pie_stop`. Model on `src/server/scripts/verify-template.ts`; copy
  `templates/verify-loop.ts` as the skeleton.
- For **unit/asset behaviour**, the loop is just `bun test <file>` (+ the pytest gate).

Then state the loop explicitly to the user (this is the guardrail, per
`docs/TESTING.md` §10):

```
1. Contract + observe-commands defined (C++ primitives added if missing)
2. Test written FIRST  →  3. implement  →  4. run headless + bounded
5. READ evidence (logs/diff/screenshot)  →  6. green=commit, red=fix → loop 3–5
```

If you want this loop to run unattended on an interval, that's `/loop` (drive
`/automated-tester` or the verify script). Don't build a bespoke scheduler.

---

## Boundaries — what this skill does NOT do

- **No mocks, no native UE automation** (`FAutomationTestBase`, Gauntlet, functional
  test actors). The MCP surface is the test API. If you reach for these, stop and add
  the missing typed command instead (§4).
- **No convenience composites** to make a test "easier" — granular primitives only;
  some composites are refused by design (`docs/ARCHITECTURE.md` §5).
- **No unbounded live-editor drivers.** Everything that touches a live editor is
  bounded + self-cleaning (timeouts, watchdog, force-`pie_stop`). Never leak a PIE
  session; one driver at a time.
- **Not a pixel-diff harness.** Screenshots are evidence for a vision/`/see` check,
  not a primary machine assertion.
- It writes tests and (when needed) the C++ primitives that make them possible — it
  does **not** redesign the feature under test or refactor unrelated code.

## Bundled files

```
.claude/skills/automated-tester/
  SKILL.md                       this playbook
  templates/integration.test.ts  Bun integration-test skeleton (AAA(O) + teardown)
  templates/verify-loop.ts       standing PASS/FAIL E2E verify-script skeleton (watchdog + finally pie_stop)
```
