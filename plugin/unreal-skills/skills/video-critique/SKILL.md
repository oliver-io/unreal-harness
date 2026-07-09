---
name: video-critique
description: >
  External, grounded critic for MOTION — judge a PIE recording of a high-change visual phenomenon
  (an animation, a VFX burst, a transition, physics in flight) against a written expected behaviour,
  the video sibling of /visual-critique's SPEC mode. Records the live game with pie_record_* and
  judges the MP4 with video_analyze (Gemini video understanding), returning a structured verdict
  (matches / diverged / inconclusive), a summary, timestamped severity-rated divergences, and
  per-criterion pass/fail results. HARD PRECONDITION: it only runs against an ARRANGED
  circumstance — game state arranged by typed C++ primitives / deterministic triggers, and a camera
  fixed by a capture rig (a parked pie_capture_from_pose, or the game's own deterministic camera).
  It refuses hand-driven play: if the scenario needs a human steering a character or aiming a
  camera, that is not this skill. Use when someone says "verify the animation plays", "does the
  door swing like the spec", "watch the jump arc / muzzle flash / dissolve and tell me if it's
  right", "record PIE and check the behaviour", "loop until the motion matches". For a SINGLE
  still frame use /visual-critique instead; for building the camera rig use /capture-pose.
---

# video-critique

A second set of eyes for **things that only exist in time** — an animation blend, a projectile arc,
a UI transition, a Niagara burst, a physics settle. A screenshot cannot judge these; your own
frame-by-frame impression of "yeah, it moved right" is exactly the confident-but-wrong self-report
this repo bans. This skill records the running game deterministically and hands the MP4 to an
external video model that reports **timestamped, severity-rated divergences** from a stated
expectation — or credits a genuine match.

It is the motion sibling of `/visual-critique` SPEC mode, and it enforces the same standard:
adversarial about the spec, but **grounded** — every cited divergence must be actually visible at
its cited timestamp; matches are a valid, useful answer. It is a second opinion, not an oracle —
cross-check surprising claims (e.g. by extracting the cited timestamp as a still and looking).

## The arranged-circumstance gate (hard precondition — refuse without it)

The VERBOTEN rule in CLAUDE.md applies to video doubly: a recording of you play-acting is a
*longer* hallucinated play-test. Before any recording, BOTH of these must hold, or you stop:

1. **The state is arranged, not played.** The phenomenon must be triggered by a deterministic,
   repeatable stimulus with no spatial navigation: a typed C++ primitive, a console hook
   (`editor_console_exec`), a Blueprint/level construct that fires on PIE begin, a scripted
   `pie_inject_input_action` / `pie_send_keystrokes` fired as a *stimulus at a known time* — never
   as steering toward a place. The same trigger from the same start state must produce the same
   scenario every run. If no such trigger exists, **add the primitive or ask the human** — exactly
   the agent-observed testing doctrine (`docs/TESTING.md` in the harness repo).
2. **The camera is a rig, not a hand.** One of:
   - a saved capture pose (`projects/<game>/captures/<name>.json`, built via `/capture-pose`)
     parked for the take with `pie_capture_from_pose(..., restore: false)`, or
   - the game's own deterministic camera (a fixed menu/UI camera, a spectator pinned by game code,
     a cinematic camera the game places itself).

   No rig for the subject yet → build one first with `/capture-pose` (human frames it once) —
   no rig, no verdict.

If either leg is missing, say so and fall back to the CLAUDE.md ladder: build the missing
primitive/rig, or ask the human to drive PIE and report back. Do not "just record something."

## The loop

1. **Preflight.** Server running with `GEMINI_API_KEY`/`GOOGLE_STUDIO_API_KEY` in the repo `.env`
   (`video_analyze` errors cleanly if not). Write the expectation in concrete, *watchable* terms —
   motion, order, timing, direction, count: "door swings outward ~90° in under 1 s, no clipping
   through the frame" grades; "animation looks good" does not. Split independently-checkable
   claims into `criteria` so each gets its own pass/fail.
2. **Acquire PIE.** `pie_start` — `pie_busy` + a queue position is not a failure; call again to
   hold your place. Everything after this runs under `try/finally: pie_stop`.
3. **Park the camera** (rig leg 2): `pie_capture_from_pose` with the saved pose and
   `restore: false`, unless the game's own camera is the rig.
4. **Record.** `pie_record_start(wait_for_pie_s, fps, max_duration_s)` — default 30 fps; `fps: 0`
   (every presented frame) for fast/transient phenomena like hit flashes. Set `max_duration_s` as
   the watchdog so a wedged run can never leave an unbounded recording.
5. **Fire the stimulus** (rig leg 1) at a known moment — note the offset so you can `clip` later.
6. **Stop:** `pie_record_stop` → `result.path`, duration, dropped-frame counters. Nonzero
   `frames_dropped` on a fast phenomenon → the take may be untrustworthy; re-record before judging.
7. **Judge:** `video_analyze(path, expected_behavior, criteria?, analysis_fps?, clip_start_s/end_s?)`.
   Clip to the interesting window; sample as low as the question allows (see cost note below).
8. **Iterate like /visual-critique:** read `divergences` (timestamped, severity-rated) and failed
   `criteria_results` top-down; fix the single most severe with a real change; re-arrange,
   re-record from the same rig + trigger, re-judge. Only the phenomenon may change between runs —
   same pose, same stimulus, same timing. Stop when the verdict is `matches` and every criterion
   passes. `inconclusive` is a framing/sampling problem, not a pass — fix the rig, fps, or clip
   and re-run; never re-roll an unchanged take hoping for a kinder verdict.

For a trivial case (short take, simple trigger you can fire while it rolls), the one-shot
`pie_analyze(expected_behavior, duration_s, ...)` composite collapses steps 4–7 — but it occupies
the session for its whole duration, so anything intricate should use the explicit sequence.

## The two fps knobs (deliberately distinct — the main cost/fidelity lever)

- **Capture fps** (`pie_record_start`): which frames *exist* in the MP4. Source fidelity.
- **Analysis fps** (`video_analyze.analysis_fps`, default 1): how densely the model *samples* the
  upload. Cost ≈ 300 tokens per sampled second. A 30 s take at the default costs ~9k tokens; the
  same take sampled at 10 fps costs ~90k. Raise it only when the question genuinely needs
  sub-second resolution (a 0.2 s flash), and pair a high `analysis_fps` with a tight
  `clip_start_s`/`clip_end_s` window instead of sampling the whole take.
- A cheap prepass for long takes: judge at the default 1 fps first to *localize* the suspect
  window from the divergence timestamps, then re-analyze only that clip densely. (For questions
  that are really about one instant, extract a still at the timestamp and use `/visual-critique` —
  don't pay video rates for a picture question.)

## Foot-guns

- **Same rig every run.** If the camera pose or trigger timing drifts between iterations, score
  changes are noise. Park the identical saved pose; fire the identical stimulus.
- **Recording is lease-scoped.** You may record your own leased session or an unleased one —
  never another agent's (`pie_not_holder`). `pie_record_arm` auto-records *every* PIE session on
  the editor until disarmed — disarm when done, don't leave it armed for other agents' takes.
- **Bounded + self-cleaning.** `max_duration_s` on every take; `finally: pie_stop`; recordings
  auto-finalize on PIE end, but never rely on that as the plan.
- **Write time INTO the spec.** The model reports timestamps — give it timing to grade against
  ("within 0.5 s of the input") or you'll get motion-shape verdicts only.
- **UMG/HUD is composited into the recording** — expected overlays belong in `expected_behavior`
  ("the debug text top-left is expected") so they aren't reported as divergences.
- **It's a critic, not a profiler.** Perceived hitching in the MP4 can be encoder drops
  (`frames_dropped`), not game hitching — check the counters before filing a performance verdict.
- The upload to the provider is transient (deleted after analysis), but it *is* an external
  service — the standard external-publishing caution applies to sensitive footage.
