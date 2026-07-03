# TESTING.md — the automated, agent-observed testing doctrine

How this harness tests Unreal features: a C++-grounded, end-to-end loop where the
**engine constructs the circumstances** and the **agent observes the behaviour it
controls** — entirely through the typed MCP surface, with no mocks and no native
UE automation framework. This is the doctrine; the `/automated-tester` skill is the
opinionated playbook that applies it.

> One line: a feature is not done until a **deterministic, headless-runnable test**
> arranges its preconditions through typed primitives, exercises it, and asserts on
> **observed state read back through a different primitive** — and when arranging or
> observing needs a capability the harness lacks, you **add that capability to the
> plugin** rather than weakening the test.

---

## 1. Why it's shaped this way

We test through the **real product surface**: a real MCP client → the registry →
the bridge → the C++ plugin → a **live** UE editor. Every assertion is against real
editor state, an on-disk `.uasset`, the live PIE world, the unified log, or pixels.

- **No mocks.** Mocking the engine would test our mock, not Unreal. The thing we
  ship *is* the typed command surface, so the test drives exactly that.
- **No native UE automation.** No `FAutomationTestBase`, no Gauntlet, no
  functional-test actors. Those live *inside* the editor and can't be authored or
  read by an external agent mid-loop. Instead, **the MCP command surface is the test
  API** — the same surface an agent uses to build the game is the one it uses to test
  it. (See `docs/ARCHITECTURE.md` §3: the typed surface is deliberately complete; the
  test harness is downstream of that completeness, not a parallel system.)
- **C++ grounds the scenario.** The "arrange" and "observe" steps are typed,
  replayable bridge commands handled on the game thread — not fragile external
  scripting poking at engine internals. That determinism is what makes a run
  repeatable and an assertion trustworthy. "We observe the action we control."

When you find yourself unable to **arrange** a precondition or **observe** an
outcome through a typed command, that is a harness gap, not a reason to lower the
bar: add the command to the plugin (`docs/USAGE.md` §3 is the C++ author's
contract). This is the autonomy principle (`CLAUDE.md` → Design philosophy).

---

## 2. The loop: Arrange → Act → Observe → Assert

Every test, at every tier, is the same four beats. The **Observe ≠ Act** rule is
load-bearing.

1. **Arrange** — bring the world to a known state with typed mutators / fixtures:
   `actor_spawn`, `level_load`, `bp_create_blueprint`, `actor_set_property`,
   `pie_start`. Make it **idempotent** (`ensureAbsent` before a create) and
   **namespaced** (`/Game/__MCPTest__/...`) so the test re-runs against a long-lived
   editor.
2. **Act** — perform the one operation under test (often with the smallest possible
   change). For mutators, optionally pre-flight with `dry_run: true` (see §6).
3. **Observe** — read the result back through a **different, deeper** primitive than
   the one that caused it. Mutate with `actor_set_transform`, observe with
   `actor_inspect`. Inspection is intentionally deeper than mutation
   (`docs/ARCHITECTURE.md` §3) — lean on it. A test that asserts only on the
   mutator's own echo is testing the echo, not the engine.
4. **Assert** — compare observed state to expectation. For asset creation, the
   strongest assertion is **the file exists on disk** at the mapped `Content/` path,
   not merely that the create call returned success.

```
mutate via actor_set_transform   →   observe via actor_inspect   →   assert location
create via bp_create_blueprint   →   observe via on-disk .uasset →   assert isFile()
start via pie_start              →   observe via pie_query+logs   →   assert behaviour
```

---

## 3. Two tiers of grounding

Pick the **lowest** tier that can prove the behaviour. Most features are provable in
the static tier; reserve PIE for runtime behaviour.

### Tier 1 — Static / asset (headless, `-nullrhi`)
Create/mutate assets and editor-world actors; assert via inspect + on-disk
`.uasset`. Runs with **no GPU, no window**, fast, the default CI tier. This is where
the vast majority of tests live (actor, bp, material, statetree, asset, …). Transient
actors evaporate on editor quit; namespaced assets are wiped after the session.

### Tier 2 — Dynamic / runtime (PIE)
`pie_start` → drive the running game → observe the live world → `pie_stop`. This is
the harness's headline capability. Observation channels, strongest first:

- **Structured log markers** (`editor_read_logs`) — the **primary runtime oracle**
  (see §4). Deterministic, greppable, headless-safe.
- **`pie_query`** — live world introspection: possessed pawns, player controllers +
  camera POV, actor positions/velocities. Poll it to assert motion/state.
- **`ai_get_state` / `ai_get_awareness` / `ai_get_perception`** — live AI agent
  introspection during PIE.
- **`editor_perf_snapshot`** — frame timing / memory, for perf regressions.

PIE start/stop is **asynchronous** (handlers return `starting`/`stopping`; the world
flips a few frames later). Always **poll `pie_get_state`** to a deadline — never
sleep a fixed duration. Tier-2 tests that *open a game viewport* are GUI-only under
`-nullrhi` and must be gated (`test.skipIf(!GUI)` / `@pytest.mark.gui_only`).

### Tier 2b — Pixel / render (GUI only)
`editor_screenshot` / `editor_window_screenshot` need a real RHI. Use sparingly and
gate on `GUI`. Screenshots are **evidence for a human / vision check**, not a
primary machine assertion — assert on the returned `bytes`/`file_path` and on-disk
size; verify *content* with the `/see` skill or a vision pass, not pixel diffs.
Foot-gun: the capture silently no-ops if the viewport isn't rendering — keep the
window foregrounded + realtime on (`docs/BUGS.md` GAP-007).

**Fixed capture rig — no screenshot verdict without one.** Before any
screenshot-based visual verdict, build a deterministic capture rig FIRST: a stored
pose (location/rotation/FOV/aspect) that frames the subject with **zero stateful
navigation** — no character driving, no camera flying (`CLAUDE.md` → VERBOTEN).
Inside PIE the sanctioned primitive is `pie_capture_from_pose`, which spawns a
transient camera at the stored pose and captures through the real game render
path; the `/capture-pose` skill turns a human-framed editor viewport into that
rig. Any later run re-captures the identical view with no human input — that
reproducibility is what upgrades a screenshot from a guess to evidence. No rig →
no verdict; fall back to asking the human to drive PIE.

**Video evidence — the second render channel.** For behaviour that only reads over
time (motion, VFX, UI transitions), record the live PIE viewport:
`pie_record_start` → drive the scenario → `pie_record_stop` lands a real `.mp4`
(machine-assert on `frames_encoded`/`bytes` and the on-disk file first), then judge
the clip with `video_analyze`. Same discipline as log markers: **state the
expectation up front** — write the `expected_behavior` and criteria *before*
recording, so the vision oracle grades a pre-committed contract instead of
rationalizing whatever it happens to see. Tool contracts, the arm/disarm
auto-record mode, the one-shot `pie_analyze` composite, and `video_analyze`'s
server-side API-key requirement live in `docs/USAGE.md` §2.18 — don't duplicate
them here. Exercised end-to-end by `tests/integration/test_pie.py` (record
lifecycle → valid MP4 container). GUI-only, like screenshots.

---

## 4. Structured log markers — the primary runtime oracle

The most robust way to assert runtime behaviour is to make the system-under-test
**emit deterministic, greppable log lines**, then assert on them via
`editor_read_logs`. Screenshots and frame-timing are noisy; a log marker is exact.

**Convention:** instrument the feature with a stable, machine-parsable prefix:

```
[FEATURE] event=goal_scored team=1 score=2 t=12.4
[FEATURE] event=match_over winner=1
```

Then the test asserts the markers appeared in order / with the right fields. This
turns "did the thing happen?" into a string match instead of a guess. A game using
this pattern emits stable `[GAME]` markers (e.g. a terminal `MATCH OVER`), and its
`verify-<game>.ts` greps for them. **Formalize this for every runtime feature:** decide the markers *before*
writing the test, instrument the feature, assert on them. One unified, sequenced log
(`MCP_Unified.log`) makes this reliable.

---

## 5. Self-driving over synthetic input

Prefer a system that **drives itself** and is observed, over a test that injects
synthetic input and hopes it lands.

- **Synthetic keystrokes are unreliable** for polled input: `pie_send_keystrokes`
  doesn't reach `IsInputKeyDown`-style polling because the PIE viewport doesn't hold
  sustained Slate focus (`docs/BUGS.md` GAP-030). Use them only to assert the
  *injection op itself* succeeds, not as the engine of a behaviour test.
- **Make the subject autonomous.** A bot that drives algorithmically, an auto-tick
  Blueprint, or a placed orchestrator actor produces motion/state you can verify via
  `pie_query` positions + log markers — deterministically, with tooling only.
- **Orchestrate from a placed actor, not the GameMode.** GameMode `BeginPlay` does
  not reliably fire in PIE (`docs/BUGS.md` GAP-031); drive the scenario from a placed
  `BP_*Director` actor instead.

---

## 6. Dry-run as a spec check

Most mutators accept `dry_run: true` and return `result.diff` instead of applying.
**Validation parity is the invariant: a passing dry-run implies a passing commit
absent races — only the apply step is skipped** (`docs/USAGE.md` §1.4). So a dry-run
test proves the operation *would* validate and shows *what would change*, with zero
side effects. Mutators that don't support it return `error_code: dry_run_unsupported`
(a clear "not ready" signal, not a silent no-op). Dry-run is the safety mechanism in
place of undo (`docs/ARCHITECTURE.md` §5) — there is no undo to lean on.

---

## 7. Determinism, safety, isolation (non-negotiables)

- **Boot gate, not sleep.** The TCP listener binds long before the editor is
  interactive. Poll `mcp_status` until `ready: true` (the harness clients already do
  this); never `sleep(n)` to "wait for boot."
- **Crash guard.** After any risky op, `assertReady()` — if the editor died, fail
  *here* with a clear message instead of cascading.
- **Bounded + self-cleaning.** Anything that drives a live editor must be bounded and
  never leak a PIE session: socket timeouts on bridge round-trips, `--timeout` on
  `bun test`, an `afterEach`/`finally` that force-`pie_stop`, and a wall-clock
  watchdog on standalone PIE scripts (`CLAUDE.md` → Run / test).
- **One editor driver at a time.** Two agents/clients driving one editor concurrently
  crashes it. Serialize. (Editor *relaunch* is safe and doesn't drop the MCP session;
  *server* restart does — reconnect with `/mcp`.)
- **Idempotent + namespaced.** `ensureAbsent` before create; scratch under
  `/Game/__MCPTest__/`; transient actors for throwaway state; teardown that never
  fails the run.
- **The test project is a persistent fixture, not an ephemeral copy.** Isolation is
  *which project* + *which namespace*, not a clone-per-run: the harness builds and
  boots `tests/fixtures/TestProject` (override: `UE_MCP_TEST_PROJECT`) and confines
  all writes to `/Game/__MCPTest__/`. Nothing ever copies a game.
- **Tests only ever attach to the test project's editor.** Both harnesses guard the
  attach path with the editor's own `project_context` identity: the Bun
  `editorSuite` treats an editor hosting any *other* project (someone's real game)
  as not-live and skips, and pytest `--ue-attach` refuses to run. Target a specific
  project deliberately via `UE_MCP_TEST_PROJECT`; `UE_MCP_ATTACH_ANY=1` bypasses the
  guard entirely (you own the consequences).
- **A test launch owns the machine-level environment.** When the harness launches
  its own editor, anything already holding the bridge port — a zombie test editor
  *or another project's live editor* — is stopped first (precise port-owner
  tree-kill, then a full socket-release wait). Don't run tests and drive a real
  game's editor on the same bridge port at the same time.

---

## 8. Coverage is a scoreboard, not a gate

Every bridge operation should have a test, and the pytest side keeps score. Mark
each test with `@covers("op_a", "op_b")`; `tests/integration/test_zz_coverage.py`
statically scans the integration sources for `@covers(...)` literals and compares
the union against `tests/harness/operations.py` — a **generated manifest** of every
bridge operation (server-sendable wire names ∩ C++ dispatch keys, plus `ping`;
regenerate with `tests/tools/regen_operations.py` after adding a command). The
check fails while any manifest op lacks a test — and it is **expected to be red
until coverage is complete**: the failure message *is* the scoreboard, a completion
percentage plus the exact list of uncovered ops (~246/281 as of 2026-07). It also
catches typo'd op names (a `@covers` name absent from the manifest fails
immediately). Server-local tools (`catalog_*`, `video_analyze`, composites, …)
never touch the bridge and are excluded from the manifest by construction; live
legacy wire overrides (e.g. `bp_add_node` sends `add_blueprint_node`) appear under
their wire names.

> **Known asymmetry:** the Bun suite (`src/server/test/`) **mirrors** the pytest
> tests but does **not** yet enforce a coverage gate. When you add a command, add the
> `@covers` pytest test (keeps the oracle green) *and* mirror it into the Bun suite
> for the fast in-process path. Closing this gap (a Bun coverage oracle) is a
> standing harness improvement.

---

## 9. Where tests live, and which to write

| Harness | Path | Transport | Use it for |
|---|---|---|---|
| **Bun in-process** | `src/server/test/` | in-memory MCP, no socket/child | **Default.** Fast, no leaks. Exercises MCP protocol → registry → handler → bridge. Editor-gated suites auto-skip when no editor is up. |
| **pytest live** | `tests/` | spawns the Bun server, owns/attaches a real editor | **The parity oracle.** Owns full lifecycle (build → link plugin → launch → await ready), enforces `@covers` coverage. The comprehensive gate. |
| **Standing verify script** | `src/server/scripts/verify-*.ts` | real MCP client vs a running editor | **Per-game headline E2E.** One bounded, PASS/FAIL script that boots PIE, drives the game, asserts markers + positions, screenshots, stops PIE. CI-runnable (`verify-template.ts` is the exemplar). |
| **One-off bridge CLIs** | `src/server/scripts/bridge-call.ts`, `bridge-batch.ts` | raw wire command(s) | Manual probing of a freshly-compiled C++ command before the server re-exposes it. Not a test; a debugging affordance. |

**Default choice:** write the Bun integration test (`editorSuite(...)`) — fast,
in-process, modern. Mirror it as a `@covers` pytest test to keep the parity oracle
honest. For a game's end-to-end behaviour, add/extend a `verify-<game>.ts`.

### Anatomy of a Bun integration test

```ts
import { test, expect, beforeAll } from "bun:test";
import { editorSuite, NS } from "../harness/suite.ts";
import { ensureAbsent, assertReady } from "../harness/ops.ts";

// editorSuite SKIPS the whole block when no interactive editor is on :55557, so
// `bun test` stays green on a dev box with no engine. ctx.mcp = in-process MCP
// client (tool names); ctx.bridge = raw bridge client (wire names).
editorSuite("myfeature", (ctx) => {
  beforeAll(async () => {
    await ensureAbsent(ctx.mcp, `${NS}/BP_Sample`);           // ARRANGE (idempotent)
    await ctx.mcp.expect("bp_create_blueprint", { name: `${NS}/BP_Sample`, parent_class: "Actor" });
  });

  test("set_property_then_inspect", async () => {
    await ctx.mcp.expect("actor_set_property", { /* ... */ });  // ACT
    const got = await ctx.mcp.expect("actor_inspect", { /* ... */ }); // OBSERVE (different tool)
    expect(got /* ... */).toBe(/* ... */);                     // ASSERT
    await assertReady(ctx.mcp);                                 // crash guard
  });
});
```

Harness helpers: `editorSuite/Ctx/LIVE/GUI/NS` (`suite.ts`),
`ensureAbsent/payload/isFalsyOrEmpty/assertReady/firstAssetOf` (`ops.ts`),
`TestClient` (`mcpClient.ts`), `RawBridge` (`bridgeClient.ts`).

---

## 10. The guided loop — tests as agent guardrails

The reason this doctrine exists: when an agent develops a feature, the automated
test **is the feedback loop that keeps it from drifting**. The loop:

```
        ┌─────────────────────────────────────────────────────────┐
        │ 1. Define the observable contract:                       │
        │    what typed command arranges it, what command + log    │
        │    markers observe it. Add C++ primitives if missing.    │
        │ 2. Write the test FIRST (or alongside) — AAA(O), headless│
        │ 3. Implement / change the feature                        │
        │ 4. Run headless: `bun test` (+ pytest for the gate)      │
        │ 5. READ THE EVIDENCE — logs, diff, screenshot, query     │
        │ 6. Green → commit. Red → fix. Loop on 3–5.               │
        └─────────────────────────────────────────────────────────┘
```

The discipline that makes the loop a *guardrail* rather than a rubber stamp:

- **The contract is defined before the implementation** — so the agent can't move
  the goalposts to match whatever it happened to build.
- **The oracle is observed state, not the mutator's echo** — so a no-op that returns
  success can't pass.
- **It runs headless and bounded** — so it can run on every change, automatically,
  without a human babysitting a GUI.
- **Failure produces readable evidence** (log markers, on-disk artifacts, the diff,
  a screenshot) the agent can act on directly — closing the loop without escalation.

Run `/automated-tester` to apply this to a specific feature.

---

## See also

- `docs/ARCHITECTURE.md` §4–§5 — what the harness holds vs refuses (undo, recipe
  libraries, black-box composites); why dry-run replaces undo.
- `docs/USAGE.md` §1 — universal contracts (auto-save, dry-run, boot gate, PIE gate).
  §3 — the C++ command author's contract (for adding arrange/observe primitives).
- `docs/BUGS.md` — known issues + workarounds.
- `src/server/scripts/verify-template.ts` — the project-agnostic end-to-end
  verification exemplar to copy into a game as `verify-<game>.ts`.
