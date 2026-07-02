#!/usr/bin/env python3
"""
see — quantitative, self-verifying image/texture analysis.

Given an image (especially a UI texture with a transparent background) it reports,
in resolution-independent fractions AND pixels:

  - true dimensions, aspect, color mode, alpha presence, DPI
  - the OPAQUE (trimmed) bounding box  — the real content vs the full canvas
  - per-edge FEATHER width             — how soft/anti-aliased each edge is
  - the SOLID INTERIOR rect            — the largest reliably-opaque region
                                          (greedy edge-peel to a coverage target)
  - the LIGHT FIELD rect               — the bright central area, distinct from a
                                          dark/burnt rim (where dark text reads)
  - a RECOMMENDED CONTENT rect         — solid interior + a safety margin, expressed
                                          as L/T/R/B insets ready for UMG OverlaySlot padding
  - shape classification + centroid
  - (optional) STRETCH-DISTORTION vs a --target WxH (the panel "stretch trap")

It then VERIFIES its own claims: numeric assertions (containment, coverage,
bbox-bounds) print PASS/FAIL and a hard failure exits non-zero, AND it renders an
annotated PNG (the image composited over a checkerboard so transparency reads,
with every computed rect drawn + labeled) for a final visual re-check by eye.

Pure analysis: it never imports assets, edits files other than its own output
PNG/JSON, or touches Unreal. Deps: Pillow + numpy (no scipy).

Usage:
  python see.py <image> [--threshold 200] [--coverage 0.99] [--margin 0.03]
                        [--lum-percentile 55] [--target 600x400]
                        [--out-dir tmp/see/<stem>] [--no-heatmap] [--json-only]
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


# ── helpers ──────────────────────────────────────────────────────────────────

def load_rgba(path):
    im = Image.open(path)
    fmt = im.format
    dpi = im.info.get("dpi")
    orig_mode = im.mode
    # 'P' (palette) may carry transparency; LA / RGBA carry alpha directly.
    has_alpha_meta = orig_mode in ("RGBA", "LA", "PA") or (
        orig_mode == "P" and "transparency" in im.info
    )
    rgba = im.convert("RGBA")
    return rgba, orig_mode, fmt, dpi, has_alpha_meta


def bbox_ge(alpha, thresh):
    """Tight (x0,y0,x1,y1) bounding all pixels with alpha >= thresh; None if none."""
    ys, xs = np.where(alpha >= thresh)
    if xs.size == 0:
        return None
    return (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)


def rect_coverage(mask, r):
    x0, y0, x1, y1 = r
    sub = mask[y0:y1, x0:x1]
    return float(sub.mean()) if sub.size else 0.0


def greedy_interior(mask, start, target, min_frac=0.08):
    """
    Largest near-opaque rectangle inside `start`, found by repeatedly peeling the
    single edge whose inner band carries the most transparency. Converges on the
    solid core while tolerating the odd transparent speck (coverage >= target),
    so a torn corner or feathered edge gets trimmed away but a tiny pinhole does
    not collapse the whole rect (vs. a strict "fully opaque" maximal rectangle).
    """
    H, W = mask.shape
    x0, y0, x1, y1 = start
    minw = max(1, int(W * min_frac))
    minh = max(1, int(H * min_frac))
    step = max(1, int(min(W, H) / 200))
    gap = 1.0 - target

    while True:
        if rect_coverage(mask, (x0, y0, x1, y1)) >= target:
            break
        prev = (x0, y0, x1, y1)
        cands = []
        if x1 - x0 > minw:
            lb = mask[y0:y1, x0:min(x0 + step, x1)]
            rb = mask[y0:y1, max(x1 - step, x0):x1]
            cands.append(("L", 1.0 - lb.mean() if lb.size else 0.0))
            cands.append(("R", 1.0 - rb.mean() if rb.size else 0.0))
        if y1 - y0 > minh:
            tb = mask[y0:min(y0 + step, y1), x0:x1]
            bb = mask[max(y1 - step, y0):y1, x0:x1]
            cands.append(("T", 1.0 - tb.mean() if tb.size else 0.0))
            cands.append(("B", 1.0 - bb.mean() if bb.size else 0.0))
        if not cands:
            break
        edge, dens = max(cands, key=lambda c: c[1])
        if dens <= gap:  # even the worst edge is already cleaner than the target gap
            break
        if edge == "L":
            x0 = min(x0 + step, x1 - minw)
        elif edge == "R":
            x1 = max(x1 - step, x0 + minw)
        elif edge == "T":
            y0 = min(y0 + step, y1 - minh)
        elif edge == "B":
            y1 = max(y1 - step, y0 + minh)
        if (x0, y0, x1, y1) == prev:  # no-progress guard
            break
    return (x0, y0, x1, y1)


def fr(rect, W, H):
    """Rect -> fractional rect {x0,y0,x1,y1} in [0,1]."""
    x0, y0, x1, y1 = rect
    return {"x0": round(x0 / W, 4), "y0": round(y0 / H, 4),
            "x1": round(x1 / W, 4), "y1": round(y1 / H, 4)}


def insets(rect, W, H):
    """Rect -> L/T/R/B insets as fractions of the canvas (UMG OverlaySlot padding)."""
    x0, y0, x1, y1 = rect
    return {"left": round(x0 / W, 4), "top": round(y0 / H, 4),
            "right": round((W - x1) / W, 4), "bottom": round((H - y1) / H, 4)}


def classify_shape(fill):
    if fill >= 0.95:
        return "rectangular"
    if 0.70 <= fill < 0.95:
        return "rounded-rect / elliptical"
    return "irregular / organic"


# ── annotation (the visual proof) ─────────────────────────────────────────────

def checkerboard(W, H, sq=24, c1=(58, 58, 62), c2=(92, 92, 98)):
    ys, xs = np.mgrid[0:H, 0:W]
    chk = (((xs // sq) + (ys // sq)) % 2).astype(bool)
    arr = np.where(chk[..., None], np.array(c2, np.uint8), np.array(c1, np.uint8))
    return Image.fromarray(arr.astype(np.uint8), "RGB").convert("RGBA")


def annotate(rgba, rects, centroid, out_path):
    """rects: list of (label, (x0,y0,x1,y1), color). Draws over a checkerboard."""
    W, H = rgba.size
    comp = Image.alpha_composite(checkerboard(W, H), rgba)
    d = ImageDraw.Draw(comp)
    try:
        font = ImageFont.load_default()
    except Exception:
        font = None
    for _, r, col in rects:
        if r:
            d.rectangle(r, outline=col + (255,), width=max(2, W // 500))
    if centroid:
        cx, cy = centroid
        m = max(8, W // 120)
        d.line((cx - m, cy, cx + m, cy), fill=(255, 0, 255, 255), width=2)
        d.line((cx, cy - m, cx, cy + m), fill=(255, 0, 255, 255), width=2)
    # legend
    ly = 6
    for label, r, col in rects:
        if not r:
            continue
        d.rectangle((6, ly, 22, ly + 12), fill=col + (255,))
        d.text((26, ly), label, fill=(255, 255, 255, 255), font=font)
        ly += 16
    comp.convert("RGB").save(out_path)


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Quantitative, self-verifying image analysis.")
    ap.add_argument("image")
    ap.add_argument("--threshold", type=int, default=200,
                    help="alpha >= this is 'solid' (0-255, default 200)")
    ap.add_argument("--any-alpha", type=int, default=10,
                    help="alpha >= this is 'present' (for feather/trim, default 10)")
    ap.add_argument("--coverage", type=float, default=0.99,
                    help="min opaque fraction for the solid interior (default 0.99)")
    ap.add_argument("--margin", type=float, default=0.03,
                    help="extra safety inset (frac of canvas) for the content rect (default 0.03)")
    ap.add_argument("--lum-percentile", type=float, default=55.0,
                    help="opaque-luminance percentile dividing 'light field' from rim (default 55)")
    ap.add_argument("--target", default="", help="WxH to test stretch distortion against, e.g. 600x400")
    ap.add_argument("--out-dir", default="", help="where to write annotated PNG + JSON")
    ap.add_argument("--no-heatmap", action="store_true")
    ap.add_argument("--json-only", action="store_true", help="print JSON only (no human summary)")
    args = ap.parse_args()

    path = Path(args.image)
    if not path.is_file():
        print(f"ERROR: not found: {path}", file=sys.stderr)
        return 2

    rgba, orig_mode, fmt, dpi, has_alpha_meta = load_rgba(path)
    W, H = rgba.size
    arr = np.asarray(rgba)
    A = arr[:, :, 3]
    RGB = arr[:, :, :3].astype(np.float32)
    has_alpha = bool(has_alpha_meta or A.min() < 255)

    out_dir = Path(args.out_dir) if args.out_dir else Path("tmp/see") / path.stem
    out_dir.mkdir(parents=True, exist_ok=True)

    report = {
        "image": {
            "path": str(path), "width": W, "height": H,
            "aspect": round(W / H, 4), "mode": orig_mode, "format": fmt,
            "has_alpha": has_alpha, "dpi": list(dpi) if dpi else None,
        },
        "params": {"threshold": args.threshold, "any_alpha": args.any_alpha,
                   "coverage": args.coverage, "margin": args.margin,
                   "lum_percentile": args.lum_percentile},
    }
    checks = []

    if not has_alpha:
        # Fully opaque: the whole canvas is content. Still report dims/aspect, and
        # a light-field within the full frame (useful for text-over-photo).
        full = (0, 0, W, H)
        report["bbox_solid"] = {"rect": full, "frac": fr(full, W, H),
                                "trimmed_size": [W, H], "note": "no alpha — full canvas is opaque"}
        solid_bbox = full
    else:
        bbox_any = bbox_ge(A, args.any_alpha)
        bbox_solid = bbox_ge(A, args.threshold)
        if bbox_solid is None:
            print("ERROR: no pixels reach the solid threshold; lower --threshold.", file=sys.stderr)
            return 2
        report["bbox_any"] = {"rect": bbox_any, "frac": fr(bbox_any, W, H)}
        bx0, by0, bx1, by1 = bbox_solid
        report["bbox_solid"] = {"rect": bbox_solid, "frac": fr(bbox_solid, W, H),
                                "trimmed_size": [bx1 - bx0, by1 - by0]}
        # per-edge feather: gap between 'present' and 'solid' bboxes
        report["feather_px"] = {
            "left": bx0 - bbox_any[0], "top": by0 - bbox_any[1],
            "right": bbox_any[2] - bx1, "bottom": bbox_any[3] - by1,
        }
        solid_bbox = bbox_solid

    # masks
    solid_mask = (A >= args.threshold)
    # centroid of opaque mass
    ys, xs = np.where(solid_mask)
    centroid = (int(xs.mean()), int(ys.mean())) if xs.size else None
    report["centroid"] = list(centroid) if centroid else None

    # shape / fill ratio
    sx0, sy0, sx1, sy1 = solid_bbox
    bbox_area = max(1, (sx1 - sx0) * (sy1 - sy0))
    fill = float(solid_mask[sy0:sy1, sx0:sx1].mean())
    report["fill_ratio"] = round(fill, 4)
    report["shape"] = classify_shape(fill)

    # solid interior
    interior = greedy_interior(solid_mask, solid_bbox, args.coverage)
    icov = rect_coverage(solid_mask, interior)
    report["interior_solid"] = {"rect": list(interior), "frac": fr(interior, W, H),
                                "insets": insets(interior, W, H), "coverage": round(icov, 4)}

    # recommended content rect = interior + safety margin (keeps content off the rim)
    mx = int(W * args.margin)
    my = int(H * args.margin)
    cx0 = min(interior[0] + mx, interior[2] - 1)
    cy0 = min(interior[1] + my, interior[3] - 1)
    cx1 = max(interior[2] - mx, cx0 + 1)
    cy1 = max(interior[3] - my, cy0 + 1)
    content = (cx0, cy0, cx1, cy1)
    report["content_recommended"] = {"rect": list(content), "frac": fr(content, W, H),
                                     "insets": insets(content, W, H),
                                     "note": "L/T/R/B insets = UMG OverlaySlot padding fractions"}

    # light field (bright central area for dark text), distinct from a dark rim
    light = None
    if solid_mask.any():
        lum = 0.299 * RGB[:, :, 0] + 0.587 * RGB[:, :, 1] + 0.114 * RGB[:, :, 2]
        opaque_lum = lum[solid_mask]
        lum_thresh = float(np.percentile(opaque_lum, args.lum_percentile))
        light_mask = solid_mask & (lum >= lum_thresh)
        if light_mask.any():
            light = greedy_interior(light_mask, solid_bbox, max(0.9, args.coverage - 0.05))
            report["light_field"] = {"rect": list(light), "frac": fr(light, W, H),
                                     "insets": insets(light, W, H),
                                     "lum_threshold": round(lum_thresh, 1),
                                     "coverage": round(rect_coverage(light_mask, light), 4)}

    # symmetry (is the content centered?)
    li = report["interior_solid"]["insets"]
    report["symmetry"] = {
        "horizontal_balanced": abs(li["left"] - li["right"]) < 0.03,
        "vertical_balanced": abs(li["top"] - li["bottom"]) < 0.03,
        "lr_inset_delta": round(abs(li["left"] - li["right"]), 4),
        "tb_inset_delta": round(abs(li["top"] - li["bottom"]), 4),
    }

    # optional stretch-distortion check
    if args.target:
        try:
            tw, th = (int(v) for v in args.target.lower().split("x"))
            sxf, syf = tw / W, th / H
            distortion = abs(sxf / syf - 1.0) * 100.0
            report["stretch_check"] = {
                "target": [tw, th], "target_aspect": round(tw / th, 4),
                "image_aspect": round(W / H, 4),
                "distortion_pct": round(distortion, 2),
                "verdict": "safe" if distortion < 1.0 else
                           ("minor" if distortion < 5.0 else "WILL DISTORT -- match aspect or 9-slice"),
            }
        except ValueError:
            print(f"WARN: bad --target '{args.target}', expected WxH", file=sys.stderr)

    # ── self-verification ──────────────────────────────────────────────────
    def chk(name, ok):
        checks.append({"check": name, "pass": bool(ok)})
        return ok

    hard_ok = True
    if has_alpha:
        # 1) solid bbox truly bounds all solid pixels
        re_bbox = bbox_ge(A, args.threshold)
        hard_ok &= chk("solid_bbox bounds all alpha>=threshold", re_bbox == solid_bbox)
    # 2) interior coverage meets target (recomputed)
    hard_ok &= chk(f"interior coverage >= {args.coverage}", icov >= args.coverage - 1e-6)
    # 3) containment: content ⊆ interior ⊆ solid bbox ⊆ canvas
    def contains(outer, inner):
        return (outer[0] <= inner[0] and outer[1] <= inner[1]
                and outer[2] >= inner[2] and outer[3] >= inner[3])
    hard_ok &= chk("content within interior", contains(interior, content))
    hard_ok &= chk("interior within solid bbox", contains(solid_bbox, interior))
    hard_ok &= chk("solid bbox within canvas", contains((0, 0, W, H), solid_bbox))

    # annotated proof
    rects = [
        ("opaque bbox (red)", tuple(solid_bbox), (220, 40, 40)),
        ("solid interior (blue)", tuple(interior), (40, 120, 255)),
        ("content+margin (green)", tuple(content), (40, 210, 90)),
    ]
    if light:
        rects.append(("light field (cyan)", tuple(light), (0, 220, 220)))
    annotated = out_dir / f"{path.stem}.see.png"
    annotate(rgba, rects, centroid, annotated)
    hard_ok &= chk("annotated PNG written", annotated.is_file() and annotated.stat().st_size > 0)
    report["outputs"] = {"annotated": str(annotated)}

    if not args.no_heatmap:
        heat = out_dir / f"{path.stem}.alpha.png"
        Image.fromarray(A, "L").save(heat)
        report["outputs"]["alpha_heatmap"] = str(heat)

    report["checks"] = checks
    report["verified"] = bool(hard_ok)

    json_path = out_dir / f"{path.stem}.see.json"
    json_path.write_text(json.dumps(report, indent=2))
    report["outputs"]["json"] = str(json_path)

    # ── output ───────────────────────────────────────────────────────────────
    if not args.json_only:
        i = report["image"]
        print(f"== see: {i['path']} ==")
        print(f"  dims {i['width']}x{i['height']}  aspect {i['aspect']}  mode {i['mode']}  alpha={i['has_alpha']}")
        if "bbox_solid" in report:
            b = report["bbox_solid"]
            print(f"  opaque bbox {tuple(b['rect'])}  trimmed {b['trimmed_size']}  frac {b['frac']}")
        if "feather_px" in report:
            print(f"  feather px {report['feather_px']}")
        ii = report["interior_solid"]
        print(f"  solid interior {tuple(ii['rect'])}  coverage {ii['coverage']}  insets {ii['insets']}")
        cc = report["content_recommended"]
        print(f"  CONTENT insets (UMG padding frac) {cc['insets']}")
        if "light_field" in report:
            lf = report["light_field"]
            print(f"  light field insets {lf['insets']}  (lum>={lf['lum_threshold']})")
        print(f"  shape {report['shape']} (fill {report['fill_ratio']})  centroid {report['centroid']}")
        if "stretch_check" in report:
            sc = report["stretch_check"]
            print(f"  stretch->{sc['target']}: {sc['distortion_pct']}% -> {sc['verdict']}")
        print(f"  checks: {sum(c['pass'] for c in checks)}/{len(checks)} pass   verified={report['verified']}")
        print(f"  annotated: {annotated}")
    print(json.dumps(report, indent=2))

    return 0 if hard_ok else 1


if __name__ == "__main__":
    sys.exit(main())
