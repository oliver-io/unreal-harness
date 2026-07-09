---
name: capture-pose
description: >
  Turn a human-framed editor viewport into a reproducible, automated IN-GAME screenshot. The human
  positions the level-editor camera on whatever they want to see; this records that exact pose
  (location/rotation/FOV/aspect) and reproduces it inside the running game (PIE) through the real game
  render path — no character driving, no camera flying — saving the shot so any later run can re-capture
  the identical view with zero human input. This is the sanctioned way to build the "fixed capture rig"
  that agent-observed testing doctrine requires (docs/TESTING.md in the harness repo) before any screenshot-based visual validation. Use when someone
  says "capture this view", "record my camera", "snapshot this framing in-game", "set up a repeatable
  shot of X", "make a capture rig for this", or "I framed it — grab it in the game". Pairs with
  /visual-critique for judging the captured frame (against a reference image or a written spec).
---

# capture-pose

You **cannot** play Unreal, and you must never hand-fly the PIE camera to a spot and screenshot it (that
is verboten — see CLAUDE.md). This skill is the alternative: a human — who *can* frame a shot — points the
editor viewport at the subject, and you turn that into a **deterministic, reproducible in-game capture**.

The split of labor is the whole point:
- **The human supplies the spatial judgment** — what to look at, from where, how framed. Once.
- **You supply the reproducibility** — record the pose as data, reproduce it in-game on demand forever.

## The two primitives this is built on
- **`editor_viewport_get_camera`** — reads the active *perspective* level-viewport pose: `location`,
  `rotation`, `fov`, `aspect`, `ortho`, `viewport_size`. This is the *record* step.
- **`pie_capture_from_pose`** — inside a live PIE session, spawns a transient camera at a given
  pose+FOV, swaps the player's view target to it (instant, no navigation), screenshots the game viewport,
  then restores. This is the *reproduce in-game* step. Returns `result.path` (status `captured`).

Why in-game and not just the editor viewport: the editor viewport and the running game **render
differently** (e.g. a glossy floor reads near-black in the editor viewport but bright in PIE, reflection
captures differ, exposure/post differ). "What it actually looks like in the game" only comes from PIE.

## Record a new shot (human-in-the-loop)

1. **Ask the human to frame it.** Tell them to position the level-editor *perspective* viewport on the
   subject and click in that viewport so it's the active one, then confirm. (If they're in an ortho
   view, ask them to switch to perspective — `ortho:true` in the read means FOV is meaningless.)
2. **Record the pose:** call `editor_viewport_get_camera`. If `ortho` is true, stop and ask them to
   reframe in a perspective viewport. Otherwise keep `location`, `rotation`, `fov`, `aspect`.
3. **Name + persist the shot.** Ask for a short shot name if not given. Write it to
   `projects/<game>/captures/<name>.json` (create the dir; it lives inside the gitignored game project):
   ```json
   {
     "name": "<slug>",
     "description": "<what this shot is for>",
     "location": { "x": 0, "y": 0, "z": 0 },
     "rotation": { "pitch": 0, "yaw": 0, "roll": 0 },
     "fov": 90,
     "aspect": 1.78,
     "map": "/Game/Maps/<Level>",
     "created": "<YYYY-MM-DD>"
   }
   ```
   (Determine `<game>` from the active project / `UNREAL_PROJECT_ROOT`; record the current level in `map`
   so a replay can load the right map.)
4. **Reproduce it in-game.** Acquire the PIE lease and start PIE: call `pie_start` (load `map` if it
   isn't already open). Respect the lease — `pie_busy` with a queue position is **not** a failure; re-call
   to hold your place (CLAUDE.md "PIE is leased"). Then call `pie_capture_from_pose` with the recorded
   `location`/`rotation`/`fov`/`aspect`. Read `result.path`.
5. **Confirm the rig works** (the "make sure we can capture that same view" step):
   - Verify `result.status == "captured"` and the file exists.
   - **Show the captured in-game frame to the user.**
   - **Objective framing check:** also grab the human's editor framing with
     `editor_window_screenshot {viewport:true}`, then run **/visual-critique** (IMAGE mode) with the editor shot as
     `--reference` and the in-game capture as `--render`, using
     `--focus "composition, framing, camera angle, which subject is centered and at what scale"` and
     `--context "image 1 is the editor viewport and image 2 is the in-game PIE render of the SAME camera
     pose; lighting, exposure, reflections and post WILL differ and must NOT be penalized — judge ONLY
     whether the same subject is framed the same way"`. A high framing score = the rig reproduces the
     view. (Don't expect a high *overall* match — the renders legitimately differ in look; you're
     checking framing parity, not art parity.)
   - If framing is off, the usual cause is an aspect mismatch (the game window aspect ≠ the editor
     viewport aspect). Re-run `pie_capture_from_pose` passing the recorded `aspect` (it letterboxes the
     capture to match) — this is already in step 4, but call it out if you skipped it.
6. **Clean up.** If you started PIE solely for this, `pie_stop` (releases the lease). Leave it running
   only if the user is mid-session.

The saved JSON is now the source of truth for that shot. Report the shot name, its file, and the captured
image path.

## Replay a saved shot (no human needed)

This is the payoff — any later run reproduces the exact view deterministically:
1. Read `projects/<game>/captures/<name>.json`.
2. `pie_start` (load its `map` if needed; respect the lease).
3. `pie_capture_from_pose` with the stored `location`/`rotation`/`fov`/`aspect` → `result.path`.
4. Hand the image to **/visual-critique** to judge it — IMAGE mode (against a reference image) or SPEC
   mode (against a written spec) — and/or show it to the user. `pie_stop` if you started PIE.

Use this to gate an iteration loop: change the art/material/scene → replay the shot → judge → repeat,
with the framing held perfectly constant across every iteration.

## Foot-guns
- **Perspective only.** An ortho viewport has no meaningful FOV; the reproduction will be wrong. Reframe.
- **Pass `aspect`.** Without it the capture fills the game window's aspect, which usually differs from the
  editor viewport → different vertical framing. Passing the recorded aspect letterboxes to match exactly.
- **The window must be able to composite.** `pie_capture_from_pose` grabs the OS window surface; a fully
  occluded/minimized PIE window can't produce a frame and you'll get an honest `timeout`, not a false ok.
- **It's a rig, not a playtest.** This captures a *fixed* pose. It does not — and must not — drive a
  character or fly a camera. That's the entire reason it exists.
- **Don't trust a stale pose after the world moves.** If the subject actor was moved/rebaked since you
  recorded the shot, the framing is stale — re-record. (Measure the live editor, per CLAUDE.md.)
