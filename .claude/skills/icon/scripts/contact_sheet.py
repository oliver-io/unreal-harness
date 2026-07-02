#!/usr/bin/env python3
"""contact_sheet — composite icons onto a checkerboard grid for human review.

The review gate before importing AI output: lay every generated icon on a
checkerboard (so transparency reads honestly), labelled, in one image you can
`Read` and show the user. Missing files are marked in red instead of crashing.

Usage:
    # explicit files, optional :Label per file
    python contact_sheet.py out.png a.png b.png:Shield c.png:"Health Potion"

    # every file matching a glob in a dir (label = filename stem)
    python contact_sheet.py out.png --dir assets/generated --glob "icon_*.png"

    # tune the grid
    python contact_sheet.py out.png --dir assets/generated --cols 5 --cell 192

Pure Pillow. Writes a single RGB PNG (the checkerboard makes the alpha visible).
"""

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def checker(size, sq=10, light=(210, 210, 210, 255), dark=(170, 170, 170, 255)):
    img = Image.new("RGBA", (size, size), light)
    d = ImageDraw.Draw(img)
    for y in range(0, size, sq):
        for x in range(0, size, sq):
            if (x // sq + y // sq) % 2 == 0:
                d.rectangle([x, y, x + sq, y + sq], fill=dark)
    return img


def load_font(px=14):
    for name in ("arial.ttf", "DejaVuSans.ttf", "LiberationSans-Regular.ttf"):
        try:
            return ImageFont.truetype(name, px)
        except Exception:
            continue
    return ImageFont.load_default()


def parse_spec(spec: str):
    """'path' or 'path:Label' -> (Path, label). Drive letters (C:\\...) are safe:
    we only split on the LAST ':' and only when the right side isn't a path tail."""
    if ":" in spec[2:]:  # skip a leading drive letter like C:
        head, _, tail = spec.rpartition(":")
        # treat as label only if the tail isn't itself part of a path
        if head and "/" not in tail and "\\" not in tail:
            return Path(head), tail
    return Path(spec), None


def build(items, out_path: Path, cols: int, cell: int):
    pad, label_h = 12, 22
    rows = (len(items) + cols - 1) // cols
    sheet_w = cols * cell + (cols + 1) * pad
    sheet_h = rows * (cell + label_h) + (rows + 1) * pad
    sheet = Image.new("RGBA", (sheet_w, sheet_h), (30, 30, 30, 255))
    draw = ImageDraw.Draw(sheet)
    font = load_font(14)

    missing = 0
    for i, (path, label) in enumerate(items):
        r, c = divmod(i, cols)
        x = pad + c * (cell + pad)
        y = pad + r * (cell + label_h + pad)
        bg = checker(cell)
        if path.exists():
            ic = Image.open(path).convert("RGBA")
            ic.thumbnail((cell, cell), Image.LANCZOS)
            ox = (cell - ic.width) // 2
            oy = (cell - ic.height) // 2
            bg.alpha_composite(ic, (ox, oy))
        else:
            missing += 1
            ImageDraw.Draw(bg).text((10, cell // 2), "MISSING", fill=(220, 50, 50, 255), font=font)
        sheet.alpha_composite(bg, (x, y))
        draw.text((x + 4, y + cell + 3), label or path.stem, fill=(230, 230, 230, 255), font=font)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    sheet.convert("RGB").save(out_path)
    print(f"-> {out_path}  ({len(items)} cells, {missing} missing)")


def main():
    ap = argparse.ArgumentParser(description="Composite icons onto a checkerboard contact sheet.")
    ap.add_argument("out", help="output PNG path")
    ap.add_argument("specs", nargs="*", help="image paths, each optionally suffixed :Label")
    ap.add_argument("--dir", help="directory to glob instead of (or in addition to) specs")
    ap.add_argument("--glob", default="*.png", help="glob within --dir (default *.png)")
    ap.add_argument("--cols", type=int, default=4)
    ap.add_argument("--cell", type=int, default=160)
    args = ap.parse_args()

    items = [parse_spec(s) for s in args.specs]
    if args.dir:
        items += [(p, None) for p in sorted(Path(args.dir).glob(args.glob))]
    if not items:
        print("No images given. Pass file specs or --dir.", file=sys.stderr)
        return 2

    build(items, Path(args.out), args.cols, args.cell)
    return 0


if __name__ == "__main__":
    sys.exit(main())
