# Test Coverage

Test-coverage tasks are tracked in [TASKS](./TASKS.md). This document should be kept as a living record of all the current testing tasks, and when a test is written and passing, if it doesn't need explicit confirmation, should be removed from the task list. We want to keep this list as a pristine pending-work list, and not confuse anyone with completed tasks. Another process is responsible for queueing the actual testing tasks onto the list — this loop only consumes them.

The tasks name a coverage gap — e.g. `missing integration test for XYZ` — and may span any part of the harness's test surface: the bun unit/protocol/integration tests (`src/server/test/`), the live-editor pytest parity oracle (`tests/`, including its `@covers` mapping and the `test_zz_coverage.py` gate), and any missing C++ arrange/observe primitive a test needs.

# Testing Philosophy (non-negotiable)

`docs/TESTING.md` is the doctrine and the `/automated-tester` skill is its playbook. Every test this loop produces conforms to it, without exception:

- **Integration-based, through the real product surface.** Tests drive a real MCP client → registry → bridge → C++ plugin → a live editor. The typed command surface *is* the test API; there is no parallel test system.
- **Never mocking. Prefer no test to a nasty mock or a useless unit test.** Mocking the engine tests the mock, not Unreal. If a behaviour can only be "tested" by mocking, by asserting on the mutator's own echo, or by a unit test that proves nothing about real editor state, we do not write it — we move the task to #DEFERRED with that reason. A missing test is honest; a hollow one is a lie the next agent believes.
- **Always and only for verifiables.** Arrange → Act → **Observe → Assert**, where the observation reads a verifiable piece of state (a transform, an on-disk `.uasset`, a `[FEATURE]` log marker, a `pie_query` result) through a **different, deeper primitive** than the one that acted. No character driving, no camera aiming, no "does it feel right" — behaviour that can't be grounded this way is deferred, never play-acted (`CLAUDE.md` VERBOTEN).
- **Always from a fixture.** Tests arrange their own world from a known baseline: idempotent (`ensureAbsent` before create), namespaced (`/Game/__MCPTest__/`), transient where possible, with teardown that never fails the run — so every test re-runs cleanly against a long-lived shared editor. If a precondition can't be arranged through a typed primitive, that's a harness gap: add the primitive, don't weaken the test.
- **Self-documenting.** The test states its own contract: `@covers("op", ...)` names what it proves, log markers are decided *before* implementation, the AAA(O) beats are legible in the test body, and failure produces readable evidence (markers, on-disk artifacts, diffs) an agent can act on directly.
- **Clean, modern, reliable.** Bun in-process (`editorSuite`) is the default tier; headless (`-nullrhi`) first, GUI-gated only where pixels genuinely matter; poll `mcp_status`/`pie_get_state` to a deadline — never `sleep(n)`; bounded and self-cleaning (`afterEach`/`finally` force-`pie_stop`, socket timeouts, crash guard via `assertReady`). A flaky test is a failed task, not a finished one.

If it does not already have one, we should make sure that the task document has a #DEFERRED list in case we don't know how to handle it.

# Test Loop Orchestrator

We should pull an item from the task list, and task a subagent with the following entire procedure, with our exterior loop just orchestrating and keeping track of agent progress. Each subagent should:

## Test Loop Task

Take a single task off of the list. Understand its request. If we cannot understand the task, we should move it to a #DEFERRED list in the task document, and clearly state that it was not understood or does not seem accurate at the time. We should not take any task descriptions at face value, instead, always:

1) understand the task, and define the observable contract first: what typed command arranges the scenario, what different command / log marker observes the outcome
2) investigate the feature's actual implementation — the MCP server tool and the associated Unreal Engine plugin handler — and the existing tests around it
3) correlate the planned test with the **actual code and the testing philosophy above** — pick the lowest tier that proves the behaviour (static/asset before PIE), fixture-arranged, observed state not the mutator's echo, no mocks
 a) if the behaviour cannot be grounded in a verifiable (it would require driving a character or camera, synthetic input as the engine of the test, a feel/look judgment, or a mock), we should move it to the #DEFERRED list with that reason — no test is better than a bad one.
 b) if arranging or observing needs a capability the harness lacks, add the C++ arrange/observe primitive rather than weakening the test (the autonomy principle).
 c) if we cannot find the implementation code the test is meant to cover, we should **ABORT** this process and stop the loop, and explain to the user.
4) once we have a test design, double check our work and make sure the task makes sense
5) write the test, run it to green **reading the actual evidence** (logs, diffs, observed state — not just the exit code), and keep the parity pair honest — the bun `editorSuite` test mirrored by a `@covers` pytest test so `test_zz_coverage.py` stays satisfied

## One Item At a Time

We address one task at a time. We take it as an isolated request, research it, design the test, and land it passing if possible. If we cannot ground the behaviour in a verifiable or get the test reliably passing, we move it to the #DEFERRED list.

## Failure Cases or Stuck Modes

If we are failing to act or stuck in any way, we should simply **ABORT** and terminate this loop process, explaining the difficulty to the user. Remember the shared-editor rules while running: PIE is leased (`pie_busy` is a queue position, not a failure), pre-existing build errors in files we didn't edit are hands-off, and anything driving a live editor must be bounded and self-cleaning (never leak a PIE session).

## Per Item Finished

Document your changes with a git commit specific to the altered files and the intent of the changes. Then, move on to the next.

# Loop

We should continue looping until there are no more tasks in our task list, or we have encountered some kind of a hard-stop issue.

# Completion

When complete, double check our work, delete any looping chronjob, and summarize all the changes.
