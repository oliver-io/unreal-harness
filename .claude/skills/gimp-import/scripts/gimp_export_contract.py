"""
Contract-mode GIMP layer exporter + naming-convention classifier (GIMP 3.x / PyGObject).

Turns a layered .xcf authored to the /gimp-import CONTRACT (see the skill's SKILL.md)
into a co-registered PNG layer set + manifest.json, AND validates every layer against
the naming grammar so stale / mis-named layers surface instead of silently shipping.
Optionally bakes a CPU hit-mask sidecar (a UTexture2D's mip-0 bulk data is freed after
GPU upload, so runtime hit-testing cannot read the texture — it samples this file).

WHY A COMMITTED SCRIPT: a contract-mode .xcf is a LIVING asset, re-painted and
re-exported repeatedly. The export must be reproducible by any contributor, and the
layer->filename mapping (the join key into the UTexture2D assets) must never drift —
so the convention lives in committed code, never in ad-hoc per-export MCP calls.

THIS FILE IS A TEMPLATE. The skill copies it into the consuming project (e.g.
projects/<Game>/tools/gimp_export/) next to a small committed config; the copy may be
extended with project-specific reserved groups (medievalCS added road-segment layers
this way). The generic grammar:

    <SCOPE>_<MID>_<STATE>[_<NONCE>]

  SCOPE  the owning top-level group (a world-map region, a HUD cluster, a menu
         screen). May itself contain underscores — NEVER parsed positionally; it is
         the group name and is stripped as a known prefix.
  MID    optional — either a SUB-UNIT (a group child of the scope, e.g. `west`) or a
         named scope LAYER (e.g. `roads`). Absent for plain scope-level overlays.
  STATE  the state / role descriptor; may contain underscores (`selected_hovered`).
  NONCE  optional trailing integer for multiples (`town_1`, `town_2`).

Structure-driven parsing removes every underscore ambiguity:
  - SCOPE is known (the group), so it is stripped first.
  - NONCE is a trailing pure-digit token.
  - MID is a SUB-UNIT iff it equals one of the scope's group-child names; otherwise
    the leading token is a named layer and the remainder is the state.
  - FEATURE vs SUB-UNIT OVERLAY (both sub-scoped) is decided by STRUCTURE: a leaf
    INSIDE the sub-unit group is a feature; a sub-scoped leaf at SCOPE level is an
    overlay.

GROUP-LEVEL RULES
  - Top-level groups named in `skip_groups`  -> scratch, never exported.
  - The `base_group` group                   -> composited to a single PNG (static
                                                backdrop; it never toggles).
  - Any other top-level group                -> a SCOPE; every LEAF under it exports
                                                solo and classifies. Sub-unit groups
                                                are containers, never exported as one.
  - A top-level loose LAYER (e.g. FOG)       -> exports solo, clipped to canvas.

CO-REGISTRATION: PNG export composites the visible layer clipped to the canvas and
preserves alpha -> every output is a canvas-sized RGBA image with the layer at its
authored offset. Stacked in UMG they need ZERO placement math.
filename == GIMP layer name + ".png" == the engine texture join key.

INVOCATION — define EXPORT_CONFIG (a dict) before exec'ing this file:
  in-session (Filters > Python-Fu console, or gimp-mcp `call_api` exec):
      EXPORT_CONFIG = {...}; exec(open(r'<path>/gimp_export_contract.py').read())
  headless (artist's open session untouched):
      "<GIMP bin>/gimp-console-3" -idf --batch-interpreter=python-fu-eval \
          -b "EXPORT_CONFIG={...}; exec(open(r'<path>/gimp_export_contract.py').read())" --quit
      (set EXPORT_CONFIG["xcf"] so the batch instance loads the saved file)

EXPORT_CONFIG keys:
  out_dir        (required) export directory; stale *.png + manifest.json are cleaned
  xcf            optional .xcf path to load headlessly; default = first open image
  skip_groups    top-level scratch groups, default ["WIP", "DECALS", "SCRATCH"]
  base_group     composited-backdrop group name, default "BASE"; None disables
  known_states   optional scope-level state vocabulary; unknown states WARN (not fail)
  content_bbox   True -> per-export opaque-pixel bbox in the manifest (alpha>0 scan)
  mask           optional CPU hit-mask sidecar:
    out_path       (required) sidecar file — write it under Content/ so it ships/stages
    res            downsampled square grid resolution, default 256
    scope_states   scope-level states that double as hit-masks, default ["hovered"]
    sub_overlays   include every sub-unit overlay as a mask, default True
    normalize_mean optional float: scope-level mask overlays painted fainter than this
                   mean nonzero alpha are boosted to parity ON THE EXPORT DUPLICATE
                   (functional paint — highlight wash + hit-mask — has a system-constant
                   density, not per-scope artistic intent). The source .xcf is never
                   modified. None disables.

Mask sidecar format (little-endian): b'WMSK', u32 version=1, u32 count; then per mask:
u32 nameLen, name(utf-8), u32 W, u32 H, W*H alpha bytes.
"""

import os
import json
import struct
from gi.repository import Gimp, Gio

try:
    _CFG = dict(EXPORT_CONFIG)  # type: ignore[name-defined]
except NameError:
    raise RuntimeError("Define EXPORT_CONFIG (a dict) before exec'ing gimp_export_contract.py")

OUT_DIR = _CFG["out_dir"]
SKIP_GROUPS = set(_CFG.get("skip_groups", ["WIP", "DECALS", "SCRATCH"]))
BASE_GROUP = _CFG.get("base_group", "BASE")
KNOWN_STATES = set(_CFG.get("known_states", []))
CONTENT_BBOX = bool(_CFG.get("content_bbox", False))
MASK_CFG = _CFG.get("mask") or None


# -- tree helpers -----------------------------------------------------------------------

def _children(item):
    return item.get_children() if item.is_group() else []


def _leaves(item):
    out = []
    for child in _children(item):
        out.extend(_leaves(child)) if child.is_group() else out.append(child)
    return out


def _subtree(item):
    out = [item]
    for child in _children(item):
        out.extend(_subtree(child))
    return out


def _all_layers(img):
    acc = []

    def walk(items):
        for it in items:
            acc.append(it)
            if it.is_group():
                walk(it.get_children())

    walk(img.get_layers())
    return acc


def _hide_all(layers):
    for it in layers:
        it.set_visible(False)


def _show_with_ancestors(layer):
    cur = layer
    while cur is not None:
        cur.set_visible(True)
        cur = cur.get_parent()


def _rect(layer):
    off = list(layer.get_offsets())  # (non_empty, x, y)
    return [off[1], off[2], layer.get_width(), layer.get_height()]


# -- the naming-convention parser ---------------------------------------------------------

def parse_name(name, scope, subunits):
    """Decompose a layer name into {sub, layer, state, nonce, ok}. `scope` is the known
    group prefix; `subunits` the scope's group-child names. ok=False if the name does
    not start with the scope prefix (non-conforming)."""
    prefix = scope + "_"
    if not name.startswith(prefix):
        return {"sub": None, "layer": None, "state": None, "nonce": None, "ok": False}

    tokens = name[len(prefix):].split("_")

    nonce = None
    if tokens and tokens[-1].isdigit():
        nonce = int(tokens[-1])
        tokens = tokens[:-1]

    sub = None
    if tokens and tokens[0] in subunits:
        sub = tokens[0]
        tokens = tokens[1:]

    layer = None
    # A scope-level (non-sub) overlay whose remainder is multi-token encodes a named
    # layer + state (roads_dirt -> layer=roads, state=dirt). Single token = pure state.
    if sub is None and len(tokens) > 1:
        layer = tokens[0]
        tokens = tokens[1:]

    state = "_".join(tokens) if tokens else None
    return {"sub": sub, "layer": layer, "state": state, "nonce": nonce, "ok": True}


# -- pixel helpers ------------------------------------------------------------------------

def _alpha_plane(layer):
    from gi.repository import Gegl
    buf = layer.get_buffer()
    ext = buf.get_extent()
    data = buf.get(Gegl.Rectangle.new(ext.x, ext.y, ext.width, ext.height),
                   1.0, "A u8", Gegl.AbyssPolicy.NONE)
    return data, ext


def content_bbox(layer):
    """Opaque-pixel (alpha>0) bbox in CANVAS coordinates, or None if fully transparent."""
    data, ext = _alpha_plane(layer)
    minx = miny = 1 << 30
    maxx = maxy = -1
    i = 0
    for y in range(ext.height):
        for x in range(ext.width):
            if data[i] > 0:
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
            i += 1
    if maxx < 0:
        return None
    return [ext.x + minx, ext.y + miny, maxx - minx + 1, maxy - miny + 1]


def normalize_alpha(leaf, target_mean):
    """Bring an under-painted functional overlay up to the canonical wash density
    (target_mean nonzero alpha), preserving its shapes: each value scales by
    target/mean, clamped at max(layer peak, target+3). No-op at >=90% of target.
    Operates on the EXPORT DUPLICATE's layer only. Returns (before, after) or None."""
    data, _ = _alpha_plane(leaf)
    total = count = peak = 0
    for v in data:
        if v > 0:
            total += v
            count += 1
            if v > peak:
                peak = v
    if count == 0:
        return None
    mean = total / count
    if mean >= target_mean * 0.9:
        return None
    k = target_mean / mean
    # Ceiling: the layer's own peak, but never below just-past-target — a layer whose
    # peak sits under the target must be allowed past it or the boost can't land.
    ceiling = max(peak, int(target_mean) + 3)
    lut = [(min(ceiling, round(v * k)) if v <= peak else v) / 255.0 for v in range(256)]
    leaf.curves_explicit(Gimp.HistogramChannel.ALPHA, lut)
    return (mean, target_mean)


# -- hit-mask sidecar ---------------------------------------------------------------------

def _downsample_nearest(alpha, w, h, ow, oh):
    """Masks are filled blobs; nearest preserves the interior — fine for hit-testing."""
    out = bytearray(ow * oh)
    for oy in range(oh):
        base = ((oy * h) // oh) * w
        for ox in range(ow):
            out[oy * ow + ox] = alpha[base + ((ox * w) // ow)]
    return out


def emit_mask_sidecar(mask_layers, path, res, canvas_w, canvas_h):
    """Write the binary alpha-grid sidecar for (name, layer) hit-masks. Returns
    (names, notes) — notes flag masks not authored full-canvas @(0,0) (registration)."""
    entries = []
    notes = []
    for name, layer in mask_layers:
        data, ext = _alpha_plane(layer)
        if (ext.x, ext.y, ext.width, ext.height) != (0, 0, canvas_w, canvas_h):
            notes.append("mask '%s' not full-canvas @(%d,%d) %dx%d — registration may be off"
                         % (name, ext.x, ext.y, ext.width, ext.height))
        grid = _downsample_nearest(data, ext.width, ext.height, res, res)
        entries.append((name, res, res, grid))

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"WMSK")
        f.write(struct.pack("<II", 1, len(entries)))
        for name, w, h, grid in entries:
            nb = name.encode("utf-8")
            f.write(struct.pack("<I", len(nb)))
            f.write(nb)
            f.write(struct.pack("<II", w, h))
            f.write(bytes(grid))
    return [e[0] for e in entries], notes


# -- export -------------------------------------------------------------------------------

def _export_visible(dup, all_layers, visible, out_path):
    _hide_all(all_layers)
    for lyr in visible:
        _show_with_ancestors(lyr)
    Gimp.file_save(Gimp.RunMode.NONINTERACTIVE, dup, Gio.File.new_for_path(out_path), None)


def export_contract(src_image, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    # Clean stale PNGs/manifest so renamed/removed layers don't leave orphans behind.
    for f in os.listdir(out_dir):
        if f.endswith(".png") or f == "manifest.json":
            os.remove(os.path.join(out_dir, f))

    dup = src_image.duplicate()
    all_layers = _all_layers(dup)

    manifest = []
    warnings = []
    mask_layers = []  # (name, leaf) overlays that double as CPU hit-masks

    mask_states = set((MASK_CFG or {}).get("scope_states", ["hovered"]))
    mask_subs = bool((MASK_CFG or {}).get("sub_overlays", True))
    norm_mean = (MASK_CFG or {}).get("normalize_mean")

    def emit(leaf, entry):
        path = os.path.join(out_dir, entry["file"])
        _export_visible(dup, all_layers, [leaf], path)
        manifest.append(entry)

    for top in dup.get_layers():
        name = top.get_name()
        if name in SKIP_GROUPS:
            continue

        if BASE_GROUP and name == BASE_GROUP and top.is_group():
            path = os.path.join(out_dir, name + ".png")
            _export_visible(dup, all_layers, _subtree(top), path)
            manifest.append({"file": name + ".png", "kind": "base", "scope": None,
                             "rect": _rect(top)})
            continue

        if not top.is_group():
            # Top-level loose layer (e.g. a full-map FOG veil) — export solo, clipped
            # to the canvas, like BASE. Consumed as a single texture, not per-scope.
            emit(top, {"file": name + ".png", "kind": "loose", "scope": None,
                       "rect": _rect(top)})
            continue

        scope = name
        subunits = [c.get_name()[len(scope) + 1:] for c in top.get_children()
                    if c.is_group() and c.get_name().startswith(scope + "_")]
        sub_groups = {c.get_name(): c for c in top.get_children() if c.is_group()}

        # Leaves inside a sub-unit group are FEATURES; leaves at scope level, OVERLAYS.
        in_group_leaves = set()
        for c in top.get_children():
            if c.is_group():
                for lf in _leaves(c):
                    in_group_leaves.add(lf)
                if not c.get_name().startswith(scope + "_"):
                    warnings.append("NON-CONFORMING sub-group (no scope prefix): '%s' in %s"
                                    % (c.get_name(), scope))

        for leaf in _leaves(top):
            lname = leaf.get_name()
            p = parse_name(lname, scope, subunits)
            if not p["ok"]:
                warnings.append("NON-CONFORMING (no scope prefix): '%s'" % lname)
            kind = ("feature" if leaf in in_group_leaves
                    else "sub_overlay" if p["sub"]
                    else "overlay")
            if (kind == "overlay" and p["ok"] and p["layer"] is None
                    and KNOWN_STATES and p["state"] not in KNOWN_STATES):
                warnings.append("unknown scope state '%s' in '%s'" % (p["state"], lname))

            if MASK_CFG and ((kind == "sub_overlay" and mask_subs)
                             or (kind == "overlay" and p["state"] in mask_states)):
                if kind == "overlay" and norm_mean:
                    # Functional paint (highlight wash + hit-mask): normalize density on
                    # the export dup, BEFORE both the PNG save and the mask bake.
                    boosted = normalize_alpha(leaf, float(norm_mean))
                    if boosted:
                        print("  normalized '%s': mean alpha %.1f -> ~%.0f"
                              % (lname, boosted[0], boosted[1]))
                mask_layers.append((lname, leaf))

            entry = {"file": lname + ".png", "kind": kind, "scope": scope,
                     "sub": p["sub"], "layer": p["layer"], "state": p["state"],
                     "nonce": p["nonce"], "rect": _rect(leaf)}
            if CONTENT_BBOX:
                entry["content_bbox"] = content_bbox(leaf)
            emit(leaf, entry)

        # Sub-unit group footprints — the bbox fallback for off-paint hit-testing.
        for gname, g in sub_groups.items():
            manifest.append({"file": None, "kind": "sub_group", "scope": scope,
                             "sub": gname[len(scope) + 1:] if gname.startswith(scope + "_") else gname,
                             "rect": _rect(g)})

    masks_meta = None
    if MASK_CFG:
        mask_names, mask_notes = emit_mask_sidecar(
            mask_layers, MASK_CFG["out_path"], int(MASK_CFG.get("res", 256)),
            src_image.get_width(), src_image.get_height())
        warnings.extend(mask_notes)
        masks_meta = {"file": MASK_CFG["out_path"], "res": int(MASK_CFG.get("res", 256)),
                      "names": mask_names}

    dup.delete()
    out = {"canvas": [src_image.get_width(), src_image.get_height()],
           "manifest": manifest, "warnings": warnings}
    if masks_meta:
        out["masks"] = masks_meta
    with open(os.path.join(out_dir, "manifest.json"), "w") as fh:
        json.dump(out, fh, indent=2)
    return out


_xcf = _CFG.get("xcf")
if _xcf:
    _img = Gimp.file_load(Gimp.RunMode.NONINTERACTIVE, Gio.File.new_for_path(_xcf))
    print("LOADED: %s %dx%d" % (_xcf, _img.get_width(), _img.get_height()))
else:
    _img = Gimp.get_images()[0]

_out = export_contract(_img, OUT_DIR)
print("CONTRACT EXPORT -> " + OUT_DIR)
for m in _out["manifest"]:
    bits = [m["kind"], m["file"] or "(group)"]
    for k in ("scope", "sub", "layer", "state", "nonce"):
        if m.get(k) is not None:
            bits.append("%s=%s" % (k, m[k]))
    print("  " + "  ".join(str(b) for b in bits))
print("count=%d  warnings=%d" % (len(_out["manifest"]), len(_out["warnings"])))
if "masks" in _out:
    print("masks=%d (%dx%d) -> %s" % (len(_out["masks"]["names"]), _out["masks"]["res"],
                                      _out["masks"]["res"], _out["masks"]["file"]))
for w in _out["warnings"]:
    print("  WARN: " + w)
if _xcf:
    _img.delete()
