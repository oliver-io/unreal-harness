---
name: icon
description: >-
  Produce a UI icon end-to-end for any UE project — pick the art style (object / glyph / glyph-mono), generate the art (GPT image gen → background removal → downscale to a crisp transparent PNG), review it on a checkerboard contact sheet, import it as a correctly-configured UTexture2D (sRGB + EditorIcon), then wire/display it (data-asset field, hardcoded path, or the bundled IconWidget) and verify it renders. Handles one icon or one coherent family (a glyph set, a category set) per run. Dispatch when an icon is missing/broken/wrong, or to generate a new set. NOT a project-wide icon audit.
user-invocable: true
allowed-tools:
  - AskUserQuestion
  - Bash
  - Read
  - Write
  - Edit
  - Grep
  - Glob
  - Skill
  - Agent
  - mcp__unrealMCP__asset_textures_import
  - mcp__unrealMCP__asset_dataasset_set_property
  - mcp__unrealMCP__asset_list
  - mcp__unrealMCP__asset_open
  - mcp__unrealMCP__asset_move
  - mcp__unrealMCP__asset_fixup_redirectors
  - mcp__unrealMCP__asset_delete
  - mcp__unrealMCP__asset_save
  - mcp__unrealMCP__editor_live_coding_compile
  - mcp__unrealMCP__editor_screenshot
  - mcp__unrealMCP__editor_read_logs
---

# icon — generate, import, and display a UI icon, correctly, end to end

Take a UI icon from **idea → styled art → a correctly-configured `UTexture2D` → the
consumer wired to it → verified rendering**. Two shapes of work, same pipeline:

- **Add / repair one icon** — it's missing, broken (dead path), or the wrong art.
- **Generate one coherent set** — a related *family* in a single pass (a stat-glyph set,
  a category set). One loop + one combined review sheet, not N separate runs.

This skill is project-agnostic: it bakes in the *pipeline and the craft*, and takes the
project-specific bits (theme/palette, folder, consumer) as inputs. It is **not** a
project-wide audit/reconcile pass.

## The model: each icon = four aligned layers

A correct icon lands all four; a bug is any misalignment:

1. **Identity** — style (`object` / `glyph` / `glyph-mono`) + canonical name + folder.
2. **Source art** — a small transparent PNG in the style its use demands.
3. **Engine asset** — a `UTexture2D` imported with correct UI texture settings.
4. **Wiring / display** — a real consumer resolves to it and it renders.

A run is **done** only when the asset is imported **and** a real consumer resolves to it
**and** it renders — not when the texture merely exists.

## Two-process boundary

- **Generation + review** are host-side (`Bash` → the bundled Python). Needs
  `OPENAI_API_KEY` and Pillow. **Credential:** ensure `OPENAI_API_KEY` is set in the
  environment (or a gitignored `.env`). In the harness repo specifically, a `PreToolUse`
  hook (`.claude/hooks/openai-cred-gate.py`) gates the generation command when no key is
  configured — store it once with `printf %s '<key>' | scripts/openai-key.sh set`, then
  re-run. Review (`contact_sheet.py`) and `--skip-gen` need no key.
- **Import + wire + verify** need the **editor running** over the MCP bridge.

---

## Step 1 — Classify (style + name + folder)

| Style (`--style`) | What it's for | Reads at |
|---|---|---|
| `object` (default) | a tangible thing — item, weapon, ability, pickup | tooltip / wheel size |
| `glyph` | bold flat symbolic emblem — stat marks, UI affordance symbols | ~16–24 px |
| `glyph-mono` | single solid-color silhouette, **tinted at runtime** (alpha is the shape) | ~16–24 px |

Pick a canonical lower-or-Pascal name your project already uses (e.g. `Icon_<Thing>` or
`T_Icon_<Thing>`) and a content folder (a `/Game/.../UI/Icons/` tree is the convention;
sort families into subfolders, keep string-referenced icons at a stable path — see the
redirector gotcha). Whether glyphs should be `glyph-mono` + runtime-tinted vs full-color
is a design choice — **ask the user** if unclear; don't assume.

## Step 2 — Investigate (find the consumer; check for dups)

1. **Find the consumer** — who will display this icon? Grep the project for how icons are
   referenced and match the existing mechanism exactly:
   - data-asset field: an `Icon` `UPROPERTY` on a `.uasset` → `asset_dataasset_set_property`.
   - hardcoded path in C++: `LoadObject<UTexture2D>(nullptr, TEXT("/Game/.../Icon_X.Icon_X"))`.
   - soft-ptr default set in a CDO ctor: `Field = FSoftObjectPath(TEXT("…"))`.
   - a widget that takes a texture at runtime (e.g. the bundled `IconWidget`, or a `UImage`).
2. **Check for dups** — `asset_list` the target folder; scan `assets/generated`; don't
   regenerate something that exists.
3. **No orphans.** Confirm something actually *displays* it. If the surface doesn't exist
   yet, either wire to a real existing consumer or flag that the surface must be built
   first (out of scope) — unless the user explicitly asks to only *generate* the asset for
   later (then import it and note it's unwired).

## Step 3 — Generate (host `Bash`) → review

```bash
S=${CLAUDE_SKILL_DIR}/scripts          # path to the bundled scripts
python $S/iconify.py "a battle axe" --out axe                    # text → object icon
python $S/iconify.py "crossed swords" --style glyph --out gw     # text → flat glyph
python $S/iconify.py "an arrowhead"  --style glyph-mono --out ah # silhouette to runtime-tint
python $S/iconify.py ./ref.png --out foo                         # reference image → redraw
python $S/iconify.py "a shield" --theme "muted iron, steel, leather, grim"  # inject palette/mood
python $S/iconify.py ./full.png --skip-gen                       # skip AI, just downscale
```

Pipeline: **generate-on-white → remove-background → downscale**. Output →
`./assets/generated/icon_<stem>.png` (override with `--out-dir`). Generating on a solid
white field first, *then* matting, yields a far cleaner cutout than asking for transparency
up front.

**Prompt-insensitive — nothing about the look is hard-coded.** Every prompt surface is an
overrideable default, so you control exactly how much direction is imposed:

| What to change | Flag |
|---|---|
| Art style | `--style object\|glyph\|glyph-mono\|none` · **`none`** imposes no direction |
| Style, inline | `--style-text "<full guidance>"` (replaces the named preset) |
| Style, your own named set | `--styles-file <json>` (a `{name: text}` map) + `--style <name>` |
| Palette / mood only | `--theme "<phrase>"` (appended to whatever style is in effect) |
| White-field framing | `--prompt-template "...{subject}...{style}..."` |
| Reference redraw | `--edit-template "...{style}..."` or `--edit-prompt "<full self-contained>"` |
| Background matte | `--matte-prompt "<full>"` |

For the most **literal, direction-free** generation, combine an empty style with a bare
template: `--style none --prompt-template "{subject}"` (then the model just draws your
subject on white). Opt into a preset (or your own via `--styles-file`) when you want a
consistent house look.

Other useful flags: `--size N` (square px, default 128), `--dims WxH` / `--trim-height N`
(crop to content for non-square art), `--gen-size 1536x1024` (wide canvas), `--model`
(override the image model). All of the above are also `run()` keyword args for programmatic use.

**Generate a SET** with a shell loop (each icon ≈ 3 API calls, ~30–60s — run in the
background for big sets):
```bash
for spec in "crossed swords:weapon" "a round shield:armor" "a healing flask:potion"; do
  python $S/iconify.py "${spec%%:*}" --style glyph --theme "your palette" --out "${spec##*:}"
done
```

**Review gate — never auto-import AI output unseen.** Composite the results onto a
checkerboard so transparency reads honestly, `Read` the sheet yourself, then show the user:
```bash
python $S/contact_sheet.py ./assets/sheet.png --dir assets/generated --glob "icon_*.png"
```
Regenerate anything off. (The user may say "skip review, just import" — honor that.) For
icons that must sit *inside a frame*, or when you need exact drawable bounds / padding, run
the **`/see`** skill on the art — it returns the opaque bounds and `content_recommended.insets`
(ready as UMG padding) and self-verifies.

## Step 4 — Import (editor MCP)

One `asset_textures_import` call (batch the whole set) into the chosen folder:
```jsonc
asset_textures_import(
  destination_folder = "/Game/<Proj>/UI/Icons/ItemTypes",
  images = [{ "path": "<abs path>/assets/generated/icon_<stem>.png",
              "name": "Icon_<Name>",
              "settings": { "sRGB": true, "compression": "EditorIcon" } }],
  force_overwrite = true,     // idempotent re-import; also how you swap art in place
)
```
- `compression:"EditorIcon"` (uncompressed) keeps small alpha-cut icons crisp — never
  default BC for UI icons.
- `sRGB:true` for color icons. For a `glyph-mono` silhouette you'll tint at runtime, sRGB
  still on; the tint is applied in the widget.
- `force_overwrite:true` is how you **swap art in place** — re-import a new PNG over an
  existing asset and every reference survives.

## Step 5 — Wire / display

| Consumer | How | Compile |
|---|---|---|
| `Icon` field on a **data-asset** `.uasset` | `asset_dataasset_set_property(asset_path, "Icon", "<asset path>")` then `asset_save` | none |
| Hardcoded `LoadObject` path in a `.cpp` body | `Edit` the path to the canonical asset | live-coding (body) |
| Soft-ptr default in a CDO ctor | set `= FSoftObjectPath(TEXT("…"))` in the ctor — **not** `ConstructorHelpers::FObjectFinder` (cook hazard) | live-coding **then editor restart** (CDO default) |
| A runtime widget | feed the loaded `UTexture2D` to the widget | — |
| **No binding field yet** | add the `UPROPERTY` (header) → rebuild gate (`AskUserQuestion`) → set value | full rebuild |

**Display helper — `templates/IconWidget.{h,cpp}`.** A drop-in, pure-C++ UMG widget that
shows a `UTexture2D` at a caller-driven size with the small-icon craft baked in: aspect
preservation, a runtime **tint** (for `glyph-mono`), and optional fractional **content
insets** (feed it `/see`'s `content_recommended.insets` so an icon lands on a frame's face,
not its rim). Copy both files into a project module, replace `ICONWIDGET_API` with your
`YOURMODULE_API`, and drive it: `SetIcon()` · `SetIconColor()` · `SetPreserveAspect()` ·
`SetContentInsets()` · `SetIconSize()` (call each layout pass). It needs no UMG asset.

## Step 6 — Verify

Confirm the asset exists (`asset_list` / `asset_open`) **and** the reference resolves
**and** it renders. Drive the UI to the state that shows it and `editor_screenshot`. If it
doesn't appear, `editor_read_logs` for missing-asset warnings. For CDO-default / once-init
wiring, verify **after** the editor restart — a green live-coding compile is not the value
taking effect.

---

## Conventions & gotchas (generic, do not break)

- **UI texture settings:** `sRGB:true` + `compression:"EditorIcon"` (uncompressed) for
  crisp small icons. Never default BC compression for UI.
- **Redirectors don't rewrite C++ string literals or `.ini` tags.** An icon referenced by
  a hardcoded `LoadObject`/`FSoftObjectPath` string must be named/placed right the first
  time; if you must move one, `asset_move` → `asset_fixup_redirectors`, **and** grep +
  rewrite the string literals by hand. Keep string-referenced icons at a stable path;
  folder only *unwired* icons freely.
- **Two-process boundary.** Generation/review = host Python; import/wire/verify = editor MCP.
- **Header change ⇒ full rebuild.** A new/changed `UPROPERTY` → the stop/build/relaunch gate
  (`AskUserQuestion`). A `.cpp` body change → live-coding.
- **Don't overwrite in-use art on a blanket decision.** Existing art is default-preserve;
  confirm before swapping. Generated PNGs are git-recoverable if tracked.
- **Review before import; confirm before deletes.** `asset_delete` an orphan only after
  confirming nothing references it.

## Bundled files

```
icon/                       # ${CLAUDE_SKILL_DIR}
  SKILL.md
  scripts/iconify.py        generate → bg-remove → downscale (OpenAI gpt-image-1; Pillow)
  scripts/contact_sheet.py  review gate: composite icons on a checkerboard with labels
  templates/IconWidget.h    drop-in C++ UMG icon widget (size/aspect/tint/insets)
  templates/IconWidget.cpp
  requirements.txt          host Python deps (openai, httpx, python-dotenv, Pillow)
```

## Related skills

- **`/see`** — measure an icon/frame's real geometry (opaque bounds, drawable interior,
  stretch risk) before placing or sizing it; hand `content_recommended.insets` to
  `IconWidget::SetContentInsets`.
- **`key-indicator-helper`** — the sibling "display a glyph on a keycap" widget; same
  bundled-template pattern.

## Boundaries

One icon or one coherent family per run · not a project-wide reconcile · human review
before import · confirm before deletes or overwriting in-use art · generation is host-side,
import/wire/verify is editor MCP.
