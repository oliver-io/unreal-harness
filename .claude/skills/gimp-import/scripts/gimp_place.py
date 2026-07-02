#!/usr/bin/env python3
"""gimp_place.py — the deterministic core of the `gimp-import` skill.

Given a set of FULL-CANVAS layer exports from a GIMP composition (each PNG is the
layer composited onto the full transparent canvas, exported with flatten=FALSE),
this:

  1. measures each layer's true opaque content rectangle on the shared canvas
     (alpha-THRESHOLD bbox — NOT ImageMagick `-trim`, which faint stray pixels
     defeat, inflating the frame and shrinking the on-screen render),
  2. tight-crops each to that rectangle (soft alpha preserved),
  3. converts every rectangle into a UMG CanvasPanelSlot anchor box expressed as
     viewport fractions of the canvas — the SAME ruler for every element, so the
     GIMP relative layout survives the import (the only artifact is a uniform
     canvas-aspect -> viewport-aspect stretch, applied equally to all),
  4. for each GROUP, also reports the group's bounding rect and each member's
     anchor RELATIVE to that group rect, so a consumer can nest the members under
     one movable parent and they stay locked together exactly as in GIMP.

It is self-verifying: every emitted fraction is asserted into [0,1], every member
rect is asserted inside its group rect, and the cropped PNG's size is asserted to
match the measured rect. A failed assertion exits non-zero.

Standalone layers (group == null) are emitted with their own canvas-fraction
anchor too, but the caller is expected to treat them as independent (place freely)
rather than locking them to anything.

Usage:
    python gimp_place.py spec.json            # spec on disk
    python gimp_place.py - < spec.json        # spec on stdin

spec.json:
{
  "canvas":   [1536, 1024],          # GIMP canvas WxH (the shared ruler)
  "threshold": 50,                    # alpha%% >= this counts as solid (default 50)
  "out_dir":  "C:/.../assets/hud",    # where tight PNGs are written
  "elements": [
    {"name":"BOTTOM_HUD","png":".../_BOTTOM_HUD.png","group":"BOTTOM","import_name":"T_HUD_Bottom"},
    {"name":"FULL_BAR",  "png":".../_FULL_BAR.png",  "group":"BOTTOM","import_name":"T_HUD_BarFill"},
    {"name":"TOP_HUD_L", "png":".../_TOP_HUD_L.png", "group":null,    "import_name":"T_HUD_Top_L"}
  ]
}

Output: a plan JSON on stdout (also written to <out_dir>/_place_plan.json). Each
element carries `tight_png`, `content_rect`, `anchor`, `size_px`; each group
carries its `rect`, `anchor`, and members with `rel_anchor`.
"""
import json
import os
import sys

try:
    from PIL import Image
    import numpy as np
except ImportError:
    sys.exit("gimp_place.py needs Pillow + numpy (pip install pillow numpy)")


def content_bbox(png_path, thresh_frac):
    """Tight bbox of pixels with alpha >= threshold, in the image's own pixel space.

    The image is assumed to be a full-canvas export, so its pixel space == canvas
    space. Returns (x0, y0, w, h) or None if fully transparent.
    """
    im = Image.open(png_path).convert("RGBA")
    a = np.asarray(im)[:, :, 3]
    mask = a >= int(round(thresh_frac * 255))
    if not mask.any():
        return None, im
    ys, xs = np.where(mask)
    x0, x1 = int(xs.min()), int(xs.max())
    y0, y1 = int(ys.min()), int(ys.max())
    return (x0, y0, x1 - x0 + 1, y1 - y0 + 1), im


def frac_anchor(rect, canvas):
    x, y, w, h = rect
    cw, ch = canvas
    mn = [x / cw, y / ch]
    mx = [(x + w) / cw, (y + h) / ch]
    for v in (*mn, *mx):
        assert -1e-6 <= v <= 1 + 1e-6, f"anchor fraction {v} out of [0,1] for rect {rect} canvas {canvas}"
    return {"min": [round(mn[0], 6), round(mn[1], 6)], "max": [round(mx[0], 6), round(mx[1], 6)]}


def rel_anchor(member_rect, group_rect):
    """Member rect expressed as fractions of the group's bounding rect (0..1)."""
    mx, my, mw, mh = member_rect
    gx, gy, gw, gh = group_rect
    mn = [(mx - gx) / gw, (my - gy) / gh]
    mxx = [(mx + mw - gx) / gw, (my + mh - gy) / gh]
    for v in (*mn, *mxx):
        assert -1e-6 <= v <= 1 + 1e-6, f"member {member_rect} not inside group {group_rect} (frac {v})"
    return {"min": [round(mn[0], 6), round(mn[1], 6)], "max": [round(mxx[0], 6), round(mxx[1], 6)]}


def union(rects):
    x0 = min(r[0] for r in rects)
    y0 = min(r[1] for r in rects)
    x1 = max(r[0] + r[2] for r in rects)
    y1 = max(r[1] + r[3] for r in rects)
    return (x0, y0, x1 - x0, y1 - y0)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "-"
    spec = json.load(sys.stdin if src == "-" else open(src, encoding="utf-8"))

    canvas = spec["canvas"]
    thresh = spec.get("threshold", 50) / 100.0
    out_dir = spec["out_dir"]
    os.makedirs(out_dir, exist_ok=True)

    elements = []
    for idx, el in enumerate(spec["elements"]):
        rect, im = content_bbox(el["png"], thresh)
        if rect is None:
            sys.exit(f"element '{el['name']}' is fully transparent at threshold {thresh*100:.0f}%")
        x, y, w, h = rect
        tight = im.crop((x, y, x + w, y + h))
        out_png = os.path.join(out_dir, f"{el['import_name']}.png")
        tight.save(out_png)
        assert tight.size == (w, h), "crop size mismatch"
        elements.append({
            "name": el["name"],
            "import_name": el["import_name"],
            "group": el.get("group"),
            # ZOrder mirrors input order, which the caller MUST order bottom->top to
            # match GIMP stacking (a static backdrop below its dynamic fill).
            "zorder": idx,
            "tight_png": out_png.replace("\\", "/"),
            "content_rect": [x, y, w, h],
            "size_px": [w, h],
            # Mode A (WYSIWYG default): point-anchored native-pixel slot. Set the slot
            # Anchors min==max==(0,0), Alignment (0,0), and these offsets verbatim ->
            # an undistorted native_w x native_h element at the GIMP canvas position.
            "native_offsets": {"Left": x, "Top": y, "Right": w, "Bottom": h},
            # Mode B (full-screen-fractional): canvas-fraction stretch box.
            "anchor": frac_anchor(rect, canvas),
        })

    # Build group rollups: bounding rect + per-member relative anchor.
    groups = {}
    for g in sorted({e["group"] for e in elements if e["group"]}):
        members = [e for e in elements if e["group"] == g]
        grect = union([m["content_rect"] for m in members])
        groups[g] = {
            "rect": list(grect),
            "anchor": frac_anchor(grect, canvas),
            "members": [
                {"name": m["name"], "import_name": m["import_name"],
                 "rel_anchor": rel_anchor(m["content_rect"], grect)}
                for m in members
            ],
        }

    # Aspect-distortion check (Reliability contract §1). If target_aspect is given
    # (e.g. 16/9), report the uniform horizontal stretch fraction anchors will apply.
    plan = {"canvas": canvas, "threshold_pct": round(thresh * 100), "elements": elements, "groups": groups}
    canvas_aspect = canvas[0] / canvas[1]
    aspect = {"canvas_aspect": round(canvas_aspect, 4)}
    if spec.get("target_aspect"):
        ta = float(spec["target_aspect"])
        stretch = ta / canvas_aspect
        aspect.update({
            "target_aspect": round(ta, 4),
            "horizontal_stretch": round(stretch, 4),
            "faithful": abs(stretch - 1.0) < 0.01,
            "note": ("pixel-faithful — canvas matches target aspect" if abs(stretch - 1.0) < 0.01
                     else f"each element will render {stretch:.3f}x wide; author the canvas at "
                          f"the target aspect (or use native-pixel mode) to remove distortion"),
        })
    plan["aspect"] = aspect
    plan_path = os.path.join(out_dir, "_place_plan.json")
    with open(plan_path, "w", encoding="utf-8") as f:
        json.dump(plan, f, indent=2)
    plan["_plan_path"] = plan_path.replace("\\", "/")
    print(json.dumps(plan, indent=2))


if __name__ == "__main__":
    main()
