---
name: key-indicator-helper
description: >-
  Render a "press this key" indicator on screen — a glyph (letter / SPACE / SHIFT / LMB) painted on a key-shaped tile (a keycap, square, or pill texture). Use when a project needs keypress hints, input prompts, a controls overlay, a tutorial "press W/A/S/D" row, or any "show the player which key to press" UI. Owns the hard part — placing a glyph on a beveled cap's TOP FACE (not its box center), sizing the font and cap so it reads as a label, swapping square↔wide by label length, and an optional looped press-down call-to-action. Genericized to any project: it references no specific keycap art — you supply the texture and measure it with the `/see` skill. Ships a drop-in C++ UMG widget template and the layout math a consumer uses to size it.
user-invocable: true
allowed-tools:
  - Bash
  - Read
  - Glob
  - Grep
  - Skill
  - Write
  - Edit
  - AskUserQuestion
---

# key-indicator-helper — put a glyph on a key, correctly

You need to tell the player "press **F**" (or W, SPACE, SHIFT, LMB). The right way to do
that is a **rendered key indicator** — a small key-shaped texture (a keycap / square tile /
pill) with the key's glyph painted on its **top face** — not bracketed plain text like
`[F]`. This skill owns the part that is deceptively hard to get right, gives you a drop-in
widget that encapsulates it, and tells you exactly how to size and verify it. It references
**no project-specific art** — you bring the texture; the method is universal.

## Why this is harder than it looks (the intellectual core)

A key-shaped texture is a small picture with a 3D-ish bevel/skirt and a flat top face. Four
traps sink the naive "drop a TextBlock in the middle" approach:

1. **The drawable face is NOT the box center.** The bevel/skirt pushes the flat top face
   **up** (and often slightly sideways). Center the glyph on the box and it sits on the skirt
   or below the face. You must anchor to the **measured face center** — a `(cx, cy)` fraction
   you get from the `/see` skill, confirmed by eye on its annotated PNG.
2. **A capital letter's ink sits high in its line box.** Even anchored to the face center, the
   glyph reads as floating. **Nudge the anchor down ~3% of the box** so the *ink* (not the
   line box) centers on the face.
3. **The glyph is a label, not a fill.** Size the font to **~⅓ of the cap-box height**
   (`FontOfBox ≈ 0.30`). Bigger and it looks like a filled tile; the cap should read larger
   than its glyph.
4. **Transparent padding lies about size.** The texture canvas usually has transparent margin
   around the opaque cap. To make the **visible** cap match a target on-screen height, divide
   by the **opaque-body fraction** (`trimmed_height ÷ canvas_height`, from `/see`) when you
   compute the box height — otherwise the cap renders smaller than you asked.

Plus two structural facts: **single-char vs multi-char** keys want different art (a square cap
for `F`, a **wide** cap for `SPACE`/`SHIFT`, with its own face center), and the cap should be
**caller-sized** — the consumer computes a box height from its own layout band and the cap
scales everything (font, press depth) from it.

The shipped template (`templates/KeyIndicatorWidget.{h,cpp}`) encapsulates all of this. Your
job is to **measure your art, supply it, and size the widget** — the traps are already handled.

## The workflow

```
1. Get / pick the key art   → a square cap texture (+ optional wide cap for SPACE/SHIFT)
2. /see it                  → face-center (cx,cy) fraction + opaque-body fraction, per texture
3. Import it                → asset_textures_import into /Game/...
4. Drop in the template     → copy KeyIndicatorWidget.{h,cpp} into the project's C++ module
5. Wire one cap             → SetCapTextures + SetFaceCenters + SetGlyph, add to a panel
6. Size it                  → SetCapSize(boxHeight) each layout pass (math below)
7. Verify                   → PIE + editor_screenshot; the glyph sits ON the face, crisp
```

### 1–2. Measure the art with `/see` (do not eyeball)

Run the **`/see`** skill on each cap texture:

```bash
python ${CLAUDE_PLUGIN_ROOT}/skills/see/scripts/see.py path/to/keycap_square.png
python ${CLAUDE_PLUGIN_ROOT}/skills/see/scripts/see.py path/to/keycap_wide.png
```

From its JSON + annotated PNG, record per texture:

- **Face center `(cx, cy)`** — start from `centroid` / `interior_solid`, then **confirm the
  top-face center by eye** on the `*.see.png` (the bevel offsets it from the box center — this
  is exactly the `see` "face-center caveat"). This is the most important number.
- **Opaque-body fraction** — `bbox_solid.trimmed_size[1] ÷ image.height` (the visible cap's
  height as a fraction of the canvas). Used in the sizing math.
- **Wide aspect** — `image.aspect` of the wide cap (the template reads this from the texture
  automatically, but you'll want it for layout).

A square cap with a beveled top face typically lands near `(0.50, 0.41)`; a wide cap near
`(0.50, 0.46)`. **Treat those as a sanity range, not a substitute for measuring yours.**

> No key art yet? Any front-on keycap/square-tile PNG with a transparent background works (a
> light cap with a dark face for dark glyphs, or a dark cap for light glyphs). To **generate**
> an on-theme cap, use the **`/icon`** skill (`--theme` to match your game) — it needs an
> OpenAI key (the `/onboard` skill sets it up, or you're prompted on first generation). Or
> hand-author / source the PNG. Either way, `/see` + this widget take over once you have it.

### 3. Import the texture

`asset_textures_import` (MCP `asset` domain) brings the PNG into `/Game/...`. Keep the import
path stable — the widget loads it by path or you pass the `UTexture2D*` in.

### 4–6. Drop in the widget, wire it, size it

Copy `templates/KeyIndicatorWidget.h` and `.cpp` into the consuming project's C++ module
(`Source/<Module>/UI/`), then make two edits:

- Replace `KEYINDICATOR_API` with the project's module API macro (e.g. `HOVERBALL_API`).
- Nothing else is required — textures, face centers, and color are passed at runtime.

Wire one cap from your HUD / widget:

```cpp
UKeyIndicatorWidget* Cap = CreateWidget<UKeyIndicatorWidget>(this, UKeyIndicatorWidget::StaticClass());
Cap->SetCapTextures(SquareTex, WideTex);                 // WideTex may be null
Cap->SetFaceCenters(FVector2D(0.50f, 0.41f),             // ← YOUR measured square face center
                    FVector2D(0.50f, 0.46f));            // ← YOUR measured wide face center
Cap->SetGlyphFromKey(EKeys::F);                          // or SetGlyph(FText::FromString("F"))
MyPanel->AddChildToHorizontalBox(Cap);                   // any panel slot
```

Then **drive its size every layout pass** from your own layout. The cap is a `USizeBox`; you
tell it a box height and it scales the font and press depth from that.

### The sizing math (consumer side)

Compute the cap-box height from the space you have, then call `SetCapSize`. Re-run on viewport
resize (in `NativeTick`, when `MyGeometry.GetLocalSize().Y` changes — that's the DPI-corrected
viewport height for a viewport-added widget; reliable, unlike `GetViewportSize` at construct):

```cpp
void UMyPromptWidget::NativeTick(const FGeometry& G, float Dt)
{
    Super::NativeTick(G, Dt);
    const float H = G.GetLocalSize().Y;
    if (H > 1.f && !FMath::IsNearlyEqual(H, LastLayoutH, 0.5f)) { ApplyLayout(H); LastLayoutH = H; }
}

void UMyPromptWidget::ApplyLayout(float ViewportH)
{
    // Want the VISIBLE cap to be ~6% of viewport height. Divide by the opaque-body
    // fraction (from /see) so the transparent padding doesn't shrink the visible cap.
    constexpr float VisibleCapOfViewport = 0.06f;   // tune to taste
    constexpr float OpaqueBodyFrac       = 0.695f;  // ← from /see (trimmed_h ÷ canvas_h)
    const float CapBoxH = (ViewportH * VisibleCapOfViewport) / OpaqueBodyFrac;
    Cap->SetCapSize(CapBoxH);
}
```

If the cap sits inside a **band/plaque** (a prompt strip) rather than the raw viewport, scale
relative to the band height instead: `CapBoxH = (BandPx * KeyFillOfBand) / OpaqueBodyFrac`,
where `KeyFillOfBand` (~0.4) is how much of the band's height the visible cap should fill, and
`BandPx` comes from your plaque layout. Same idea — always divide by the opaque-body fraction.

## Square vs wide caps

`SetGlyph` switches automatically: a label **longer than one character** (SPACE, SHIFT, CTRL,
ENT, LMB) uses the **wide** texture if you supplied one (with the wide face center and the
texture's own aspect); single characters use the square cap. If you pass no wide texture,
multi-char labels stay on the square cap (they'll just be cramped — supply a wide cap for a
clean result). `KeyLabel(FKey)` produces the terse cap text (`SPACE`, `SHIFT`, `LMB`, `ENT`,
or a single uppercase letter) and is `static` so a controls table can share it.

## The press-down call-to-action (the "tutorial" pattern)

For a tutorial / onboarding prompt that says "now press these," enable the looped press
animation and **phase-offset a row of caps so they ripple** instead of pressing in unison:

```cpp
// A row of caps: W A S D, rippling left-to-right.
const float RippleStep = 0.18f;   // seconds of phase offset per cap
for (int32 i = 0; i < Keys.Num(); ++i)
{
    UKeyIndicatorWidget* Cap = CreateWidget<UKeyIndicatorWidget>(this, UKeyIndicatorWidget::StaticClass());
    Cap->SetCapTextures(SquareTex, WideTex);
    Cap->SetFaceCenters(SquareFace, WideFace);
    Cap->SetGlyphFromKey(Keys[i]);
    Cap->SetPressCTAEnabled(true);
    Cap->SetPressPhaseOffset(i * RippleStep);     // ripple, not unison
    Row->AddChildToHorizontalBox(Cap);
}
```

The animation is a code-driven render-transform (down → hold → release → pause, looped) — no
`UWidgetAnimation` asset. A static prompt (e.g. an interaction "[F] Open") simply leaves the
CTA off (the default).

## Showing the player's *bound* (rebindable) key

`SetGlyphFromKey(FKey)` shows whatever key you hand it. To show the **live bound** key (so a
rebind reflects in the prompt), resolve the `FKey` from your project's input/settings system
**first**, then pass it in — e.g. read the Enhanced Input mapping for the action, or your
settings subsystem's override, falling back to the default. The widget deliberately does not
know your binding system; keep that resolution in the project and feed it the resulting `FKey`.

## Alternative route: pure MCP widget tools (no C++ / no build)

The C++ template is preferred — it paints **crisp Slate fonts** the immediate-mode HUD canvas
cannot, and encapsulates the math. But if a project can't take a C++ widget, the same *method*
works through the MCP `widget` domain (`widget_create`, `widget_add_child`,
`widget_set_property`): build a Canvas/Overlay with an `Image` (the cap texture) and a
`TextBlock` (the glyph) as siblings, and set the TextBlock's **anchor to the measured face
center** + **alignment (0.5, 0.5)** + the down-nudge, and its font size to ~⅓ of the cap box.
The hard numbers (face center, opaque-body fraction) come from `/see` either way — only the
wiring differs. Lose the press animation unless you script it via a widget animation.

## Verify (do not skip)

1. Build, run PIE, `editor_screenshot` (GUI mode for render — see `tests/run.ps1 --ue-mode=gui`).
2. **Look at it like a critic.** Is the glyph centered **on the top face**, not the skirt? Is
   it crisp (real Slate font, not blurry HUD-canvas text)? Does the **visible** cap match the
   size you intended (if it's small, your opaque-body-fraction divide is missing or wrong)? For
   a wide cap, is the multi-char label centered on the bar? If anything's off, re-measure the
   face center with `/see` and re-look — almost every defect traces to a wrong face-center
   fraction or a missing opaque-body divide.

## Boundaries & honesty

- **No project-specific art, paths, or lore.** This skill never names a particular keycap
  asset. The technique and the template are universal; the texture + its measured fractions are
  the project's to supply.
- **Measure, don't guess.** Every face-center and size number comes from `/see` (run + verified
  by eye), not from a remembered constant. The defaults in the template are a starting estimate
  for a typical beveled cap and are explicitly there to be overwritten.
- **`/see` is a hard dependency for correctness** — it's the measurement half of this skill.
  This skill is the wiring half.
