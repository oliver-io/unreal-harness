---
name: visual-critique
description: >
  External, grounded critic for a RENDER against a DESIRED STATE that is EITHER a reference IMAGE or a
  written TEXT spec — one skill, two modes. IMAGE mode (--reference <img>): art-direction comparison —
  sends the reference + the render to Gemini and returns a calibrated, rubric-scored breakdown of what
  matches and what differs, per element, with engine-specific fixes. SPEC mode (--desired "<text>"):
  adversarial spec-conformance — decomposes the written desired state into independently-checkable
  attributes ("crimson", "square", "centered") and reports DESIRED vs OBSERVED with a pass/fail verdict
  and a specific fix per attribute. Honest, not performatively harsh: every cited difference is grounded
  in what is actually visible, and genuine matches are credited. Use whenever you must judge whether a
  render matches a target — a concept image OR a description — and want a second opinion beyond your own
  eyes; drives a render -> critique -> fix loop and can gate it on a numeric score. Triggers: "compare to
  the concept", "is this close to the reference", "grade this render", "does this match the spec", "I
  asked for X — is it right", "what's wrong vs the concept art / vs what I wanted", "loop until it matches".
---

# visual-critique

A second set of eyes that is **not** invested in liking the result. Self-assessment of renders is
unreliable — a 1600px frame downscaled to a thumbnail, plus bloom, hides crushed blacks, flat materials,
missing/occluded elements, and wrong palettes, and the temptation is to call it "close." This skill
refuses to. It judges a render against whatever you can describe the target as:

- **IMAGE mode** — you have the target as a **reference image** (concept art, a target frame). It compares
  render-vs-image and scores art-direction parity, element by element.
- **SPEC mode** — you have the target **in words** (a spec, a ticket, a design line). It pulls the
  description apart into individual attributes and adversarially tries to fail each one, naming the
  precise deviation — ask for "a crimson, square, centered button" and if it came out bright-red,
  rectangular, or off-center, it tells you *that*, attribute by attribute, not a vague "looks close."

Exactly one of `--reference` (image) or `--desired` (text) is given, and that choice selects the mode.

## When to use
- After **every** meaningful visual change in a target-matching pass — before claiming progress.
- As the **gate** of an iteration loop: render → critique → fix the top blocking failure → repeat.
- Any time you're about to tell the user "this looks like the reference" or "this is what was asked for."
  Run this first.
- Pick the mode by what your target actually is: a **picture** → `--reference`; a **description** → `--desired`.

## Usage
```bash
# IMAGE mode — render vs a reference image
bun .claude/skills/visual-critique/critique.ts \
  --reference projects/<game>/assets/concept/<art>.png \
  --render    projects/<game>/Saved/Screenshots/<frame>.png \
  [--focus "floor material, panel occlusion"] [--context "ignore the debug overlay"] [--json] [--gate 85]

# SPEC mode — render vs a written desired state
bun .claude/skills/visual-critique/critique.ts \
  --desired "a crimson, square, centered button with a thin gold border" \
  --render  projects/<game>/Saved/Screenshots/<frame>.png \
  [--focus "color, centering"] [--json] [--gate 90]

# long specs: read the desired state from a file instead of inline
bun .claude/skills/visual-critique/critique.ts --desired-file spec.txt --render shot.png --json
```
- **Target (exactly one, required):** `--reference <img>` (IMAGE mode) **or** `--desired "<text>"` /
  `--desired-file <path>` (SPEC mode). `--render <img>` is always required.
- `--json` → machine-readable output. The schema is **mode-specific**:
  - IMAGE: `{match_score, verdict, matches[], blocking_failures[], elements{}, prioritized_fixes[], requires_for_90[]}`.
  - SPEC: `{conformance_score, verdict, attributes[], violations[], passes[], requires_for_pass[]}` — each `attributes[]` entry is `{attribute, desired, observed, verdict, severity, fix}`.
- `--gate N` → process exits `3` when the score is `< N` (works in both modes and both output formats). Keep a loop running until the target is met.
- `--focus "<areas>"` — areas/attributes to scrutinize hardest. `--context "<text>"` — scene ground truth that is NOT penalized and NOT listed as a difference/violation (e.g. "the debug text is expected").
- Model defaults to `gemini-3.5-flash` (fast, accurate), falling back to `gemini-3.1-pro-preview`; override with `--model`. (The older `gemini-2.5-*` models are stale — don't use them.) Each run logs `[model] … · mode=… · …` to stderr so you can verify the model, mode, and the exact images/spec sent.
- Key: `GOOGLE_STUDIO_API_KEY` in the repo `.env` (Bun auto-loads it). Never printed.

## The standard it enforces
Both modes are **rigorous AND grounded** — calibrated to what is actually visible, never confabulated:

- **IMAGE mode** scores against a calibrated, honest rubric — the model rates the band the render genuinely
  sits in (90+ = reads as the same shot; 50-69 = same intent, real divergences; 0-29 = wrong/placeholder),
  grounds every cited difference in something visible, and credits real matches. Use its `matches` +
  `blocking_failures` + `prioritized_fixes` as the work list.
- **SPEC mode** **decomposes** the desired state into independently-checkable attributes (every adjective,
  color, shape, measurement, position, count, relationship is its own attribute), verifies each against
  the image, and reports `desired` vs `observed` with a `pass`/`fail` verdict and a severity. The stance
  is **adversarial but grounded**: an attribute passes only if the render *clearly* satisfies it
  (borderline → fail, with the deviation named), yet every cited fail must be genuinely visible. Use
  `violations` + `requires_for_pass` as the work list.

A hard-won lesson behind both: an earlier prompt that ordered the model to "assume the render is wrong and
cap the score" produced confabulated, contradictory criticism (e.g. calling translucent panels "opaque
boxes"). **Don't reintroduce that.** It is a second opinion, not an oracle — cross-check its claims against
your own eyes.

## The loop
1. Capture a deterministic frame (a fixed rig pose, never a hand-flown PIE shot — see `/capture-pose`).
2. `--json --gate <N>`. Read `blocking_failures` (IMAGE) / `violations` (SPEC) top-down.
3. Fix the **single most severe** failure with a real change (custom material, post, mesh, layout) — never
   a placeholder or a value nudge that papers over it.
4. Re-capture, re-run. The score must climb. If it doesn't, your fix was wrong — revert and rethink.
5. Stop only when the gate passes AND there are no remaining blocking failures / violations.

## Foot-guns
- **Same framing every run.** Use a fixed capture rig so only the result changes between runs, not the camera.
- **Full-res in.** Don't pre-shrink the render; let the model see the real pixels.
- **Write specs in concrete, checkable terms.** "crimson, 1:1, centered" is gradeable; "looks cool" is not — vague specs get vague verdicts.
- **It's a critic, not a renderer.** It cannot see your material graph — translate its notes into engine changes yourself.
- Costs vision credits per call. Cheap relative to shipping amateur art; don't spam it on no-op changes.
