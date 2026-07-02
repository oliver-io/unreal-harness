#!/usr/bin/env python3
"""iconify — generate a game/UI icon from a text prompt or reference image.

Three-stage pipeline (the intellectual labor):
  1. GENERATE on a clean white field  — text prompt (or a reference image redraw)
     → a 1024x1024 PNG on solid white, via OpenAI `gpt-image-1`.
  2. REMOVE the background            — re-run images/edits with background=transparent
     to cut the subject out cleanly (the model does the matting; no chroma key).
  3. DOWNSCALE to an icon             — Lanczos resize + unsharp to a crisp small PNG
     with a transparent background, ready to import as a UI texture.

Generating on white first, then matting, gives a far cleaner cutout than asking for a
transparent background up front — the model commits to a solid subject, then extracts it.

PROMPT-INSENSITIVE BY DESIGN. Nothing about the look is hard-coded into the pipeline:
every prompt surface is a default you can override at runtime —
  * the art STYLE        → --style <name> (presets below) | --style none (no direction)
                           | --style-text "<your guidance>" | --styles-file <json>
  * the PALETTE / mood   → --theme "<phrase>"
  * the white-field FRAMING → --prompt-template "...{subject}...{style}..."
  * the reference redraw  → --edit-template "...{style}..."  /  --edit-prompt "<full>"
  * the background matte   → --matte-prompt "<full>"
Run with `--style none` and a bare `--prompt-template "{subject}"` for the most literal,
direction-free generation; opt into a preset (or your own) when you want a house look.

Usage:
    python iconify.py "a longsword"                       # default preset (object)
    python iconify.py "a longsword" --style none          # no imposed style/direction
    python iconify.py "crossed swords" --style glyph      # bold flat symbolic glyph
    python iconify.py "an arrowhead" --style glyph-mono   # single-color silhouette (runtime-tint)
    python iconify.py "a torch" --style-text "watercolor, soft edges, hand-painted"
    python iconify.py "a shield" --styles-file mygame.styles.json --style mygame_ui
    python iconify.py "a key" --theme "muted iron, steel, leather, grim"
    python iconify.py ./ref.png                           # reference image → redrawn icon
    python iconify.py ./full.png --skip-gen               # skip AI, just downscale assets/full.png
    python iconify.py "a banner" --dims 96x64             # crop-to-content then force exact size

Output → <out-dir>/icon_<stem>.png   (out-dir defaults to ./assets/generated).
Needs OPENAI_API_KEY (env or a .env in the cwd). Deps: openai, httpx, python-dotenv, Pillow.
"""

import argparse
import base64
import json
import os
import re
import sys
from io import BytesIO
from pathlib import Path

import httpx
from openai import OpenAI

try:
    from dotenv import load_dotenv
    load_dotenv()
except ImportError:
    pass  # python-dotenv optional; OPENAI_API_KEY may already be in the env

SIZE = "1024x1024"
MODEL = "gpt-image-1"
API_BASE = "https://api.openai.com/v1"

# ── Default prompt templates (all overrideable at runtime) ─────────────────────
# The ONLY thing the pipeline truly needs is a solid subject on a clean field it can
# matte — that's it. These defaults express that; pass your own to change anything.
# Placeholders: {subject} (your prompt) and {style} (the resolved style text).

DEFAULT_SUBJECT_TEMPLATE = (
    "A single {subject}, centered on a perfectly clean solid white background. "
    "No shadows on the background, no other objects. {style}"
)
DEFAULT_EDIT_TEMPLATE = (
    "Redraw this object centered on a perfectly clean solid white background. "
    "Keep the object identical. No shadows, no other objects. {style}"
)
DEFAULT_MATTE_PROMPT = (
    "Extract just the object from this image. Remove the background entirely, "
    "leaving only the object with a fully transparent background. Keep every "
    "detail of the object exactly as-is."
)

# ── Built-in style presets (opt-in via --style; none of them is forced) ─────────
# These are DEFAULTS you select, not behavior baked into the pipeline. Override any
# of them with --style-text, replace/extend the set with --styles-file, or pick
# "none" for no style direction at all.
#
# object     — a real, physical thing photographed on a lightbox (item/weapon/pickup).
# glyph      — a bold, flat, symbolic emblem (stat marks, UI affordance symbols).
# glyph-mono — a single solid-color silhouette on transparency, to TINT at runtime.
# none       — empty: impose no style/direction whatsoever (maximally literal).

STYLE_OBJECT = (
    "Style: realistic, photographic, physically-real. "
    "Render as a real physical object photographed on a lightbox -- real materials, "
    "real surface detail, real wear. Directional lighting with hard shadows on the "
    "object itself (never on the background). "
    "Do NOT stylize, do NOT use cartoon or illustration style, do NOT use cel shading, "
    "do NOT use painterly brushstrokes. "
    "There must be exactly one object in the image -- never two, never a pair, never crossed. "
    "The single item should fill as much of the frame as possible, centered and prominently "
    "placed. Angle the object at an isometric cant (roughly 30-45 degrees from vertical) to "
    "best show off its shape and detail at this scale. "
    "No duplicates, no reflections, no secondary objects."
)

STYLE_GLYPH = (
    "Style: a BOLD, FLAT, SYMBOLIC ICON / GLYPH -- a clean game-UI emblem designed to be "
    "instantly readable and unmistakable at tiny size (16-24 px) sitting in a UI row. "
    "Thick confident outlines, simple bold shapes, one strong silhouette, high contrast, "
    "minimal interior detail. Flat and graphic (at most very subtle shading); it must NOT "
    "look photographic, 3D-rendered, or realistic -- no scene, no perspective, no cast "
    "shadows, no background (transparent). Front-on, flat presentation -- not angled. "
    "Exactly one clear central symbol, centered, filling most of the frame, with a distinct "
    "instantly-recognizable shape. No duplicates, no text, no lettering, no border, no frame."
)

STYLE_GLYPH_MONO = (
    "Style: a SINGLE-COLOR FLAT SILHOUETTE ICON -- one bold, solid, filled shape in "
    "pure black (#000000) on a fully transparent background, and nothing else. It must "
    "read instantly and unmistakably at tiny size (16-24 px). Exactly ONE clean, simple, "
    "exaggerated silhouette with a strong distinctive outline; fill it COMPLETELY with "
    "solid black -- NO interior detail, NO shading, NO gradients, NO highlights, NO "
    "outline-only linework, NO second color, NO texture, NO cast shadow, NO background. "
    "Flat and graphic, front-on, not angled, not 3D, not photographic, not realistic. "
    "The shape's recognizable form must come purely from its outline. Center it and make "
    "it fill most of the frame. No duplicates, no text, no lettering, no border, no frame."
)

BUILTIN_STYLES = {
    "object": STYLE_OBJECT,
    "glyph": STYLE_GLYPH,
    "glyph-mono": STYLE_GLYPH_MONO,
    "none": "",          # impose nothing
    "": "",              # alias for none
}


def slugify(prompt: str) -> str:
    """Turn a prompt into a safe filename stem (first few words)."""
    return "_".join(prompt.lower().split()[:4])


def fmt(template: str, **kw) -> str:
    """Fill {subject}/{style} (only those keys), then collapse runs of whitespace so
    an empty style/subject never leaves a double space or dangling fragment."""
    out = template
    for k, v in kw.items():
        out = out.replace("{" + k + "}", v or "")
    return re.sub(r"\s+", " ", out).strip()


def load_styles(styles_file: str | None) -> dict:
    """Built-in styles, optionally overlaid with a project JSON of {name: text}."""
    styles = dict(BUILTIN_STYLES)
    if styles_file:
        custom = json.loads(Path(styles_file).read_text(encoding="utf-8"))
        if not isinstance(custom, dict):
            print(f"Error: {styles_file} must be a JSON object of {{name: style_text}}")
            sys.exit(1)
        styles.update({str(k): str(v) for k, v in custom.items()})
    return styles


def resolve_style(style_name: str, style_text: str | None, styles: dict,
                  theme: str | None) -> str:
    """The style guidance to inject. `style_text` (inline) wins over the named preset;
    `none`/empty imposes nothing; `theme` (palette/mood) is appended if given."""
    if style_text is not None:
        base = style_text
    elif style_name in styles:
        base = styles[style_name]
    else:
        avail = ", ".join(sorted(k for k in styles if k))  # includes the "none" key
        print(f"Error: unknown --style '{style_name}'. Available: {avail}")
        sys.exit(1)
    if theme:
        base = (base + f" Theme / palette / mood: {theme}.").strip()
    return base


def _api_headers() -> dict:
    return {"Authorization": f"Bearer {os.environ['OPENAI_API_KEY']}"}


def _api_edit(image_bytes: bytes, prompt: str, background: str, size: str = SIZE,
              model: str = MODEL) -> bytes:
    """Call images/edits and return raw PNG bytes, or exit with a clear message.

    `size` controls the canvas — gpt-image-1 supports 1024x1024 (default),
    1536x1024 (landscape, for wide subjects), 1024x1536 (portrait), or auto."""
    resp = httpx.post(
        f"{API_BASE}/images/edits",
        headers=_api_headers(),
        files={"image": ("input.png", image_bytes, "image/png")},
        data={"model": model, "prompt": prompt, "size": size, "background": background},
        timeout=180,
    )
    if resp.status_code != 200:
        try:
            body = resp.json().get("error", {})
            msg = body.get("message", resp.text)
        except Exception:
            msg = resp.text
        print(f"Error ({resp.status_code}): {msg}")
        sys.exit(1)
    return base64.b64decode(resp.json()["data"][0]["b64_json"])


# -- Step 1: Generate on white field -------------------------------------------

def generate_on_white(client: OpenAI, subject: str, style: str, model: str = MODEL,
                      template: str = DEFAULT_SUBJECT_TEMPLATE) -> bytes:
    """Text prompt -> 1024x1024 PNG on a clean white background."""
    result = client.images.generate(
        model=model,
        prompt=fmt(template, subject=subject, style=style),
        size=SIZE,
        output_format="png",
        background="opaque",
    )
    return base64.b64decode(result.data[0].b64_json)


def edit_on_white(image_path: Path, style: str, edit_prompt: str | None = None,
                  template: str = DEFAULT_EDIT_TEMPLATE, size: str = SIZE,
                  model: str = MODEL) -> bytes:
    """Reference image -> PNG redrawn on white via images/edits.

    `edit_prompt` (full self-contained override) wins over `template`; both may use the
    {style} placeholder. Use a custom prompt to TRANSFORM the reference (e.g. widen a
    keycap) rather than reproduce it."""
    with open(image_path, "rb") as f:
        img_bytes = f.read()
    base = edit_prompt if edit_prompt is not None else template
    return _api_edit(img_bytes, prompt=fmt(base, style=style), background="opaque",
                     size=size, model=model)


# -- Step 2: Remove background ------------------------------------------------

def remove_background(image_bytes: bytes, size: str = SIZE,
                      matte_prompt: str = DEFAULT_MATTE_PROMPT) -> bytes:
    """Take the white-field image and regenerate with a transparent background.

    Always via the default MODEL (gpt-image-1): some newer image models reject
    transparent-background edits, and the cutout is a generic extraction where the
    creative model doesn't matter. `size` must match the white-field canvas."""
    return _api_edit(image_bytes, prompt=matte_prompt, background="transparent",
                     size=size, model=MODEL)


# -- Step 3: Downscale to icon ------------------------------------------------

def downscale(image_bytes: bytes, dst_path: Path, size: int = 128,
              trim_height: int | None = None, dims: tuple[int, int] | None = None) -> None:
    """Downscale raw PNG bytes and write to dst_path (Pillow only — no ImageMagick).

    Default: a square `size`x`size` icon, keeping the generated centered framing.
    For non-square subjects, first crop to the object's tight OPAQUE bounding box,
    then:
      - `dims=(w,h)` resizes to EXACT dimensions (forces a fixed size/aspect);
      - `trim_height` resizes to that height, width proportional (keeps TRUE aspect).
    `dims` wins if both are given."""
    from PIL import Image, ImageFilter
    im = Image.open(BytesIO(image_bytes)).convert("RGBA")

    if dims or trim_height:
        mask = im.split()[3].point(lambda p: 255 if p > 10 else 0)  # opaque bbox
        bbox = mask.getbbox()
        if bbox:
            im = im.crop(bbox)
        if dims:
            im = im.resize(dims, Image.LANCZOS)
        else:
            w, h = im.size
            new_w = max(1, round(w * trim_height / max(1, h)))
            im = im.resize((new_w, trim_height), Image.LANCZOS)
    else:
        im = im.resize((size, size), Image.LANCZOS)

    im = im.filter(ImageFilter.UnsharpMask(radius=0.6, percent=80, threshold=1))
    im.save(str(dst_path))


# -- Pipeline -----------------------------------------------------------------

def run(input_arg: str, out_name: str | None = None, skip_gen: bool = False,
        style: str = "object", style_text: str | None = None,
        styles_file: str | None = None, theme: str | None = None,
        subject_template: str = DEFAULT_SUBJECT_TEMPLATE,
        edit_template: str = DEFAULT_EDIT_TEMPLATE, edit_prompt: str | None = None,
        matte_prompt: str = DEFAULT_MATTE_PROMPT, size: int = 128,
        trim_height: int | None = None, gen_size: str = SIZE, model: str = MODEL,
        dims: tuple[int, int] | None = None, out_dir: Path | None = None,
        in_dir: Path | None = None):
    styles = load_styles(styles_file)
    st = resolve_style(style, style_text, styles, theme)
    in_dir = in_dir or Path("assets")
    out_dir = out_dir or (Path("assets") / "generated")
    input_path = Path(input_arg)
    is_file = input_path.exists() and input_path.suffix.lower() in (
        ".png", ".jpg", ".jpeg", ".webp",
    )

    stem = out_name or (input_path.stem if is_file else slugify(input_arg))
    out_dir.mkdir(parents=True, exist_ok=True)
    icon_path = out_dir / f"icon_{stem}.png"

    if skip_gen:
        src = input_path if is_file else (in_dir / f"{stem}.png")
        if not src.exists():
            print(f"Error: {src} not found (--skip-gen requires it)")
            sys.exit(1)
        print(f"Skipping generation, reading {src}")
        final_bytes = src.read_bytes()
    else:
        if not os.environ.get("OPENAI_API_KEY"):
            print(
                "Error: OPENAI_API_KEY is not set — AI image generation needs an OpenAI "
                "API key.\n"
                "  One-time setup:  printf %s '<your-key>' | scripts/openai-key.sh set\n"
                "  (writes it to the gitignored .env; or run the /onboard skill).\n"
                "  Already have art? Re-run with --skip-gen to skip generation.",
                file=sys.stderr,
            )
            sys.exit(1)
        client = OpenAI()

        # Step 1: generate on white
        print(f"Generating on white field (model={model}, style={style!r})...")
        if is_file:
            white_bytes = edit_on_white(input_path, st, edit_prompt=edit_prompt,
                                        template=edit_template, size=gen_size, model=model)
        else:
            white_bytes = generate_on_white(client, input_arg, st, model=model,
                                            template=subject_template)
        print("  done")

        # Step 2: remove background
        print("Removing background...")
        final_bytes = remove_background(white_bytes, size=gen_size, matte_prompt=matte_prompt)
        print("  done")

    # Step 3: downscale and persist
    if dims:
        size_desc = f" (fixed {dims[0]}x{dims[1]})"
    elif trim_height:
        size_desc = f" (trim to {trim_height}px tall)"
    else:
        size_desc = f" to {size}x{size}"
    print(f"Downscaling{size_desc}...")
    downscale(final_bytes, icon_path, size=size, trim_height=trim_height, dims=dims)
    print(f"  -> {icon_path}")


def main():
    ap = argparse.ArgumentParser(
        description="Generate a game/UI icon from a prompt or image. Prompt-insensitive: "
                    "every prompt surface (style, framing, matte) is an overrideable default.")
    ap.add_argument("input", help="text prompt, or path to a reference image (.png/.jpg/.webp)")
    ap.add_argument("--out", help="output filename stem (default: derived from input)")
    # style selection (three independent ways; precedence: --style-text > --style)
    ap.add_argument("--style", default="object",
                    help="named style preset: object|glyph|glyph-mono|none, or a name from "
                         "--styles-file (default: object)")
    ap.add_argument("--style-text", default=None,
                    help="inline style guidance that fully replaces the named preset")
    ap.add_argument("--styles-file", default=None,
                    help="JSON {name: style_text} merged over the built-in presets")
    ap.add_argument("--theme", default=None,
                    help="palette/mood phrase appended to whatever style is in effect")
    # prompt-template overrides (the rest of the imposed text)
    ap.add_argument("--prompt-template", default=DEFAULT_SUBJECT_TEMPLATE,
                    help="white-field framing for text prompts; placeholders {subject} {style}")
    ap.add_argument("--edit-template", default=DEFAULT_EDIT_TEMPLATE,
                    help="reference-image redraw framing; placeholder {style}")
    ap.add_argument("--edit-prompt", default=None,
                    help="full self-contained reference prompt (overrides --edit-template)")
    ap.add_argument("--matte-prompt", default=DEFAULT_MATTE_PROMPT,
                    help="background-removal prompt override")
    # sizing
    ap.add_argument("--size", type=int, default=128, help="square icon size in px (default 128)")
    ap.add_argument("--trim-height", type=int, default=None,
                    help="crop to content, resize to this height keeping true aspect")
    ap.add_argument("--dims", default=None, help="crop to content, force exact WxH (e.g. 96x64)")
    ap.add_argument("--gen-size", default=SIZE,
                    help=f"generation canvas: {SIZE}|1536x1024|1024x1536|auto (default {SIZE})")
    ap.add_argument("--model", default=MODEL, help=f"image model for generation (default {MODEL})")
    # io
    ap.add_argument("--out-dir", default=None, help="output dir (default ./assets/generated)")
    ap.add_argument("--in-dir", default=None,
                    help="dir --skip-gen reads <stem>.png from (default ./assets)")
    ap.add_argument("--skip-gen", action="store_true",
                    help="skip AI generation; just downscale an existing PNG")
    args = ap.parse_args()

    dims = None
    if args.dims:
        w, h = args.dims.lower().split("x")
        dims = (int(w), int(h))

    run(args.input, out_name=args.out, skip_gen=args.skip_gen, style=args.style,
        style_text=args.style_text, styles_file=args.styles_file, theme=args.theme,
        subject_template=args.prompt_template, edit_template=args.edit_template,
        edit_prompt=args.edit_prompt, matte_prompt=args.matte_prompt, size=args.size,
        trim_height=args.trim_height, gen_size=args.gen_size, model=args.model,
        dims=dims, out_dir=Path(args.out_dir) if args.out_dir else None,
        in_dir=Path(args.in_dir) if args.in_dir else None)


if __name__ == "__main__":
    main()
