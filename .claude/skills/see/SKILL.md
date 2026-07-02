---
name: see
description: Analyze an image/texture to understand its REAL geometry — true (trimmed) dimensions ignoring transparent padding, the opaque content bounds, the solid interior that's safe to render content/text over, the bright field vs a dark/burnt rim, edge feathering, shape, and whether it will distort if stretched to a target size. Returns resolution-independent fractions + pixels as JSON AND an annotated proof PNG, and self-verifies. Dispatch BEFORE placing UI content over an image, sizing/scaling a sprite or background, choosing UMG OverlaySlot/Canvas padding for a transparent texture, deciding stretch-vs-9-slice, or any "what part of this image is actually drawable / how big is the real content / where's the transparency" question. Pure analysis — does NOT import assets, edit textures, or touch Unreal (hand its numbers to whatever wires the widget — e.g. the `key-indicator-helper` skill for keypress glyphs).
user-invocable: true
allowed-tools:
  - Bash
  - Read
  - Glob
  - Grep
  - Skill
  - AskUserQuestion
---

# see — know an image's real geometry, then prove it

You are about to place content over an image, scale it, or size a panel to it — and the
image lies. Its canvas includes transparent padding, its edges are feathered or torn, and
the parchment/paper/keycap field has a dark rim or beveled skirt you must not put text on.
**Measuring this by eye is how panels end up with text clipping a torn corner, a glyph
sitting on a key's bevel instead of its face, or a frame stretched into mush.** This skill
measures it numerically, then proves the measurement with a picture you read back with your
own eyes.

## The method in one breath

**Run the analyzer → read its JSON → look at the annotated PNG it drew → report the
numbers the caller needs (and only those).** The analyzer is self-verifying: it asserts its
own claims (containment, coverage, bbox-bounds) and exits non-zero on a hard failure, and it
renders the image over a checkerboard with every computed rectangle drawn and labeled so you
can confirm by eye that the boxes hug the right regions. Numbers can pass while a box still
visually clips a torn edge — that is why **both** layers exist and you must do both.

## Run it

```bash
python .claude/skills/see/scripts/see.py <image> [options]
```

Deps: Pillow + numpy (no scipy). This repo's test toolchain already has them; if a bare
`python` lacks them, `pip install pillow numpy`.

Common options:

| Option | Meaning |
|---|---|
| `--threshold N` | alpha ≥ N counts as "solid" (0–255, default **200**). Lower it for very soft/glowy art. |
| `--coverage F` | min opaque fraction the solid-interior rect must hit (default **0.99**). |
| `--margin F` | extra safety inset (fraction of canvas) for the recommended content rect (default **0.03**) — keeps content off the feathered/burnt rim. |
| `--lum-percentile P` | opaque-luminance percentile splitting the bright "light field" from a dark rim (default **55**). Raise to demand a brighter text bed; lower to include more of the paper. |
| `--target WxH` | report stretch distortion if the image were drawn at this size (the panel "stretch trap"). |
| `--out-dir DIR` | where to write the PNG + JSON (default `tmp/see/<stem>/`). |
| `--no-heatmap` / `--json-only` | skip the alpha heatmap / suppress the human summary. |

## What it gives you

All rects come as **fractions** (resolution-independent — use these for UMG) and pixels.

- **`image`** — true `width`×`height`, `aspect`, `mode`, `has_alpha`, `dpi`.
- **`bbox_solid`** — the opaque content's tight bounding box + **`trimmed_size`**: the real
  content dimensions ignoring transparent padding. This is the answer to "how big is it
  actually" for scaling, and (for a keycap) the ratio `trimmed_height / canvas_height` is the
  **opaque-body fraction** you divide by to make the *visible* cap match a target size.
- **`feather_px`** — per-edge soft-edge width (gap between "any alpha" and "solid"). Wide
  feather ⇒ inset content further.
- **`interior_solid`** — the largest reliably-opaque rectangle (`coverage` reports how
  opaque), with `insets` (L/T/R/B fractions). The measured safe-to-draw-on region.
- **`content_recommended`** — `interior_solid` + `--margin`. Its **`insets` map directly to
  a UMG `OverlaySlot` padding** so content lands on the paper, not the rim.
- **`light_field`** — the bright central area, distinct from a dark/aged rim — where *dark
  text* reads with most contrast. Narrower/off-center than `content_recommended`; use it when
  contrast matters more than space, and read its `insets` knowing it can bias toward the
  lighter side of the art.
- **`shape`** (+`fill_ratio`), **`centroid`** (pivot/centering), **`symmetry`** (is the
  content centered?), and **`stretch_check`** (`distortion_pct` + verdict) when `--target`
  given.
- **`outputs`** — paths to the annotated PNG, alpha heatmap, and full JSON.
- **`checks` / `verified`** — the self-verification results.

> **The face-center caveat for keys / beveled tiles.** `see` measures the *opaque* and
> *bright* regions, which for a 3D-looking keycap is the whole cap (skirt + top face), not the
> flat top face alone. The drawable **face center is usually offset from both the box center
> and the centroid** (the bevel/skirt pushes it up, sometimes sideways). For those, take
> `interior_solid` / `light_field` / `centroid` as the starting estimate, then **confirm the
> exact face center by eye on the annotated PNG** and record it as a `(cx, cy)` fraction. That
> fraction is the single most important number the `key-indicator-helper` skill needs.

## The verification discipline (do not skip the second half)

1. Run the analyzer. Confirm `verified: true` and every `checks` entry passed. A non-zero
   exit means a hard assertion failed — fix inputs (e.g. lower `--threshold`) before trusting
   anything.
2. **`Read` the annotated `*.see.png` and look at it like a critic.** Zoom into a corner.
   Does the blue solid-interior box sit fully on opaque paper? Does the green content box
   clear the burnt rim? Does the cyan light-field actually land on the light area / the key's
   top face? If a box clips an edge or floats off the art, re-run with a different
   `--threshold` / `--coverage` / `--lum-percentile` / `--margin` and look again. Iterate one
   knob at a time.
3. Report only the numbers the caller asked for, citing them as fractions, and hand off.

## Interpreting for common jobs

- **Content/text over a transparent background** → use `content_recommended.insets` as the
  `OverlaySlot`/Canvas padding; check `light_field` if the art has a dark rim and text must pop.
- **Glyph centered on a key / square tile** → see the face-center caveat above; report the
  `(cx, cy)` face-center fraction and the opaque-body fraction (`trimmed_size` ÷ canvas).
- **Scaling a sprite/background to fit** → use `bbox_solid.trimmed_size` (not the canvas) and
  preserve `image.aspect`; run `--target` to confirm you won't distort.
- **Centering / pivot** → `centroid` and `bbox_solid` center.
- **Stretch vs 9-slice decision** → `--target`; a "WILL DISTORT" verdict means match the
  aspect or nine-slice instead of stretching.

## Boundaries & honesty

- **Analysis only.** `see` never imports a texture to `/Game`, edits an asset, or drives
  Unreal. It writes only its own PNG/JSON under the out-dir. Importing the texture
  (`asset_textures_import`) and wiring the result into a widget is the consumer's job — for a
  keypress/key glyph that's the **`key-indicator-helper`** skill; hand it the fractions.
- **Every reported value is measured and cross-checked — never estimated.** If the defaults
  don't fit the art (e.g. a glow with no hard edge), say so and tune the thresholds rather
  than reporting a number you didn't verify.
- Deps: Pillow + numpy (no scipy). Pure-CPU, single image.

## Extending see

The analyzer is the place to add new "exactly-similar" measurements as they come up — keep
each one self-verified (a numeric assertion + something visible in the annotated PNG). Likely
next axes: connected-component / sprite-atlas island reporting (multiple opaque regions),
nine-slice margin auto-suggestion (uniform-border detection), and premultiplied-vs-straight
alpha + edge-fringe detection. Add the flag, draw the result on the proof image, and assert it.
