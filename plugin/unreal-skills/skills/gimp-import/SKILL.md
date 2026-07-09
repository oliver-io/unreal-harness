---
name: gimp-import
description: >-
  Import a layered GIMP composition (.xcf) into Unreal as correctly-configured UTexture2D assets and wire it up — in one of two modes. MOCKUP mode recreates a GIMP-authored UI/HUD layout as a UMG widget one time (tight-crop + anchor math, the user fine-tunes after). CONTRACT mode establishes a LIVING layered asset — a layer-NAMING GRAMMAR, co-registered full-canvas exports with zero placement math, a COMMITTED reproducible export script + manifest, and optional CPU hit-mask/bounding-box sidecars for irregular-shape interaction (maps, state overlays, drill-down UI). Use for "import the HUD from GIMP", "bring my GIMP layers into Unreal", "re-import the touched-up art", "set up a layered map/overlay pipeline", "make the GIMP export repeatable", "hit-test the painted regions". Driven live over the GIMP MCP + UnrealMCP, or headlessly via gimp-console-3. NOT an art generator — it moves existing layers; pair with /icon or /see for art/measurement.
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
  - mcp__gimp-mcp__check_server
  - mcp__gimp-mcp__call_api
  - mcp__gimp-mcp__list_images
  - mcp__gimp-mcp__list_layers
  - mcp__gimp-mcp__get_image_metadata
  - mcp__gimp-mcp__set_layer_properties
  - mcp__gimp-mcp__export_image
  - mcp__unrealMCP__asset_textures_import
  - mcp__unrealMCP__asset_list
  - mcp__unrealMCP__widget_tree_read
  - mcp__unrealMCP__widget_create
  - mcp__unrealMCP__widget_add_child
  - mcp__unrealMCP__widget_set_property
  - mcp__unrealMCP__pie_start
  - mcp__unrealMCP__pie_stop
  - mcp__unrealMCP__pie_get_state
  - mcp__unrealMCP__editor_screenshot
  - mcp__unrealMCP__mcp_status
---

# gimp-import — layered GIMP art → UE textures, two modes

Take art authored as **GIMP layers on one canvas** and land it in Unreal as
correctly-configured `UTexture2D`s. There are **two distinct products** — decide which
one the user needs before doing anything:

| | **MOCKUP mode** | **CONTRACT mode** |
|---|---|---|
| The art is | a one-shot layout to recreate | a **living asset**, re-painted + re-exported many times |
| Semantics come from | layer *structure* (group = coupled, loose = free) | a **naming grammar** the layers follow |
| Export framing | tight-crop each element, place with anchor math | **full-canvas co-registered** — zero placement math |
| Export driver | this session, live over the GIMP MCP | a **committed script** in the consuming project |
| In UMG | one Image per element, slot math per element | sibling full-canvas Images, toggled by **visibility** |
| Interaction | none (static presentation) | optional **CPU hit-mask + bbox sidecars** for irregular shapes |
| Re-import story | agent re-derives decisions | **filename = layer name = join key**; repaint → re-export, no re-wire |
| Right for | a HUD composition the user will hand-tune after import | maps, state overlays (hover/selected), fog, drill-down UI, anything with *interaction states painted as layers* |

**When in doubt ask one question: "will an artist repaint and re-export this?"** Yes →
CONTRACT. No, it's a one-time recreate → MOCKUP. (This split is lifted from a
production project whose region-overlay world map ran the contract for years:
committed exporter, manifest validation, mask sidecars — and whose doc explicitly
*rejects* ad-hoc per-export MCP calls as unreproducible drift.)

**Prerequisites.** The `mcp__gimp-mcp__*` tools come from
[maorcc/gimp-mcp](https://github.com/maorcc/gimp-mcp) — an external MCP server this
repo doesn't bundle (GIMP 3.2+, plugin installed, **Tools → MCP → Start MCP Server**,
TCP `:9877`; install walkthrough: `/onboard`). If `check_server` fails, that setup is
what's missing. CONTRACT mode can also run **fully headless** via `gimp-console-3`
batch (no GIMP UI, no MCP server, artist's open session untouched) — see §Contract.

## Reading the real group tree (both modes — do NOT trust `is_group`)

The gimp-mcp server's `list_layers` / `get_image_metadata` report **`is_group: true`
for every layer** (they test `hasattr(layer,'get_children')`, always true in GIMP 3.x)
and enumerate **only top-level** layers — nested children are invisible to them and to
`set_layer_properties`' name lookup. **Read the tree yourself via `call_api`**, which
runs Python in GIMP's live context:

```python
# call_api(api_path="exec", args=["exec", ["<this, as one \n-joined string>"]])
import json
img = Gimp.get_images()[0]
def walk(layers, parent=None):
    out = []
    for L in layers:
        g = bool(L.is_group())               # Gimp.Item.is_group() — the reliable check
        out.append({"name": L.get_name(), "is_group": g, "parent": parent,
                    "offsets": list(L.get_offsets())[1:],   # (_, x, y) -> [x, y]
                    "w": L.get_width(), "h": L.get_height(), "vis": L.get_visible()})
        if g:
            out += walk(list(L.get_children()), L.get_name())
    return out
print("GROUPTREE:" + json.dumps(walk(img.get_layers())))
```

You only ever export **leaf** layers — a group is an organizational node, not a graphic.

---

# CONTRACT mode — the living layered asset

The core inversion vs MOCKUP: **the canvas is the coordinate system, and you keep it.**
Every layer exports onto the full transparent canvas at its authored offset, so every
PNG is the same size and every pixel is already in place. In UMG the layers are
**sibling `Image`s filling one identical rectangle**, drawn in Z-order, toggled by
`SetVisibility` — *zero* per-layer positioning math, which deletes the entire
crop/threshold/anchor error surface. The engine's only job is deciding **which
pre-authored layers are visible**; every visual state is exactly what the artist drew.

## 1. The naming grammar

Layer names encode semantics. The canonical grammar (parse **structurally**, never
positionally — underscores inside names must never break parsing):

```
<SCOPE>_<MID>_<STATE>[_<NONCE>]
```

- **`SCOPE`** — the owning **top-level group** (a map region, a HUD cluster, a menu
  screen; e.g. `NORTH_FOOTHILLS`). Known from the tree, stripped as a prefix.
- **`MID`** — optional: a **sub-unit** (a group child of the scope, e.g. `west`) or a
  named scope **layer** (e.g. `roads`). Absent for plain scope overlays.
- **`STATE`** — the role/state descriptor; may itself contain underscores
  (`selected_hovered`).
- **`NONCE`** — optional trailing integer for multiples (`town_1`).

| Pattern | Meaning | Example |
|---|---|---|
| Top-level group | a **scope** | `NORTH_FOOTHILLS` |
| `<SCOPE>_<state>` (full-canvas @0,0) | scope **state overlay** | `NORTH_FOOTHILLS_hovered` |
| `<SCOPE>_<layer>_<state>` | scope **named-layer overlay** | `NORTH_FOOTHILLS_roads_dirt` |
| `<SCOPE>_<sub>_<state>` (leaf at scope level) | **sub-unit overlay** | `NORTH_FOOTHILLS_west_selected_hovered` |
| nested group `<SCOPE>_<sub>` | a **sub-unit** | `NORTH_FOOTHILLS_west` |
| `<SCOPE>_<sub>_<type>_<n>` (leaf *inside* the sub-group) | a **feature** icon | `NORTH_FOOTHILLS_west_town_1` |
| `BASE` group | static backdrop, **composited to ONE png** | — |
| loose top-level layer | full-canvas veil, exports solo | `FOG_OF_WAR` |
| `WIP`/`DECALS`/`SCRATCH` groups | never exported | — |

Feature vs sub-unit overlay (both sub-scoped) is disambiguated by **structure**: a
leaf *inside* the sub-group is a feature; a sub-scoped leaf at *scope* level is an
overlay. Sub-unit membership is decided by matching the scope's group-child names —
never by guessing at underscores.

**Setting up a fresh asset:** propose this grammar to the user, map their layers onto
it (renames in GIMP are cheap *now*, expensive after textures/registries bind to the
names), and record any project-specific vocabulary. **Adopting an existing asset:**
derive the vocabulary from the tree walk and confirm it.

## 2. The join key + registration invariants (the two load-bearing rules)

- **Filename = layer name = join key.** `.xcf` layer name → PNG filename →
  `UTexture2D` asset name → whatever registry/C++ binds textures to roles. Keep names
  stable across repaints and re-export is a reimport-in-place with **no code touched**.
- **Registration geometry:** every state overlay is authored **full-canvas at offset
  (0,0)**. Oversized groups clip to the canvas on export; positioned feature icons
  export onto the canvas at their authored offset. A full-canvas overlay moved off
  (0,0) **de-registers** — the whole stack misaligns. The exporter warns on this.

Artist-action → cost table (tell the user this — it's the iteration promise):

| Artist action in the `.xcf` | Effect on re-export | Action required |
|---|---|---|
| Repaint an existing layer | same filename → reimport replaces in place | none |
| Add a layer (grammar-conforming) | new texture, unreferenced | bind it in the consumer |
| Rename a layer | old asset orphaned, new name unmapped | update the binding (or rename back) |
| Resize the canvas | every texture changes size | re-export all; normalized UV math survives |
| Move a full-canvas overlay off (0,0) | de-registers | restore to (0,0) |

## 3. The committed export script — never ad-hoc

**Ad-hoc per-export MCP calls are rejected in contract mode.** They aren't
reproducible by other contributors and invite drift in filenames/sizes that silently
breaks the join key. Instead, **copy the skill's template into the consuming project**
and commit it:

- Template: `${CLAUDE_SKILL_DIR}/scripts/gimp_export_contract.py` — implements
  the grammar parser, full-canvas per-leaf export, BASE compositing, scratch skipping,
  stale-PNG cleanup, `manifest.json` emission, **loud warnings for every
  non-conforming layer**, and the optional mask/bbox sidecars. Config-driven
  (`EXPORT_CONFIG` dict); the header documents every key.
- Destination: `projects/<Game>/tools/gimp_export/` (or the project's tools dir) with
  a small committed driver that sets `EXPORT_CONFIG` (out_dir, skip_groups, mask
  config, known_states) and `exec`s the template copy. Project-specific reserved
  groups (e.g. a `_travel` road-segment group) are extensions the copy owns.
- Run it either way — results are identical:
  - **in-session**: via gimp-mcp `call_api` `exec`, against the artist's open image;
  - **headless**: `"<GIMP bin>/gimp-console-3" -idf --batch-interpreter=python-fu-eval
    -b "EXPORT_CONFIG={...,'xcf': r'<path>'}; exec(open(r'<driver>').read())" --quit` —
    loads the *saved* `.xcf` into its own instance; the artist's session is untouched.
- **Read the manifest after every export.** `warnings` non-empty = stale/mis-named
  layers about to ship; surface them, don't paper over them. `manifest[]` entries
  carry `kind`/`scope`/`sub`/`state`/`nonce` plus each layer's authored `rect`
  (`content_bbox` optional) — this is the machine-readable model of the art.

## 4. Masks + bounding boxes — interacting with irregular shapes

A Slate slot is an axis-aligned rectangle; painted regions are irregular blobs, and in
a co-registered stack every Image covers the whole canvas — per-widget hover/click
events are useless. Cursor→shape resolution is done by **sampling alpha in canvas
space** (inverse any pan/zoom transform first).

- **A `UTexture2D` is NOT CPU-readable at runtime** — mip-0 `BulkData` is freed after
  the RHI upload (verified: empty even in PIE). Hit-testing must read a **sidecar**,
  never the texture.
- The exporter's `mask` config bakes one: a binary `WMSK` file (per-mask downsampled
  alpha grid, default 256²) written **under `Content/`** so it ships and stages when
  cooked. Sub-unit overlays and configured scope states (default `hovered`) double as
  their own masks — lightly alpha-filled painted shapes need no separate mask art
  (alpha > 0 = inside).
- **Paint first, bounds fallback.** The manifest's `rect`/`content_bbox` and
  `sub_group` footprints are the *off-paint fallback* (e.g. an icon just outside the
  painted wash), tighter box first. Bounds must never outrank paint — icon-group
  bboxes and painted territory drift apart freely as art iterates.
- **Functional paint has a system-constant density.** When an overlay is highlight
  *and* hit-mask *and* (say) fog-punch at once, per-scope alpha whims break the system;
  the exporter's `normalize_mean` boosts under-painted scope masks to a canonical mean
  **on the export duplicate only** — the source `.xcf` is never modified.
- Loading `WMSK` + sampling is ~30 lines of consumer C++ (read header, cache flat
  arrays, index `[y*res+x]` scaled from canvas UV). The skill emits the sidecar; the
  consumer wires the hit-test, like all runtime behaviour.

## 5. Import + consume in UE

- **Stop PIE first** (`pie_active` blocks asset + widget mutation).
- `asset_textures_import` all PNGs, `force_overwrite:true`, one call. For painted UI
  art the load-bearing settings are:
  ```json
  "settings": { "sRGB": true, "compression": "EditorIcon",
                "lod_group": "UI", "mip_gen": "NoMipmaps" }
  ```
  `lod_group:"UI"` — UI-group textures are **never streamed**, so the full mip is
  always resident; without it the art streams in and renders a **blurry low mip on
  first open**. `mip_gen:"NoMipmaps"` — drawn near 1:1, a single full-res mip can't
  show a blurry lower mip and mips waste memory. (`EditorIcon` = uncompressed RGBA;
  block compression muddies thin painted boundaries. For huge always-scaled-down art,
  `Default` + `lod_group:"UI"` is an acceptable size trade.)
- Namespace per scope: `/Game/<Project>/UI/<AssetName>/<Scope>/`.
- **UMG consumption model:** one container (the pan/zoom/clip transform lives on the
  container, never per layer) → sibling full-canvas `Image`s in GIMP stacking order
  (bottom = lowest ZOrder), `DrawAs: Image`, visibility driven by state. Two traps:
  **every layer widget needs a unique name** (`ConstructWidget` with a reused name
  silently replaces the object — siblings collapse into one, last texture wins), and
  keep visibility rules as **pure functions of state** (testable headless) rather than
  scattered imperative toggles.

---

# MOCKUP mode — one-shot layout recreate

For a composition the user will hand-tune after import. Semantics come from structure:
**a layer GROUP is a coupled unit** whose members keep their relative geometry (a bar
trough + its fill); **an ungrouped layer is an independent graphic** placed freely. Ask
which group members are **runtime-driven** (a draining fill, a needle) unless a name
hint says so (convention: leading `~` or `dyn:`).

1. **Plan.** Tree-walk (above), classify leaves, confirm the layer→widget mapping and
   destination names.
2. **Export — flatten static, split dynamic.** A group's never-moving members
   composite into **one backdrop texture** (layout baked into pixels is immune to
   every anchor/threshold/Z error); each dynamic member and each standalone layer
   exports isolated on the full canvas. Mechanics per export: make exactly the
   intended layers visible (nested layers via `call_api` — name lookup can't reach
   them), `export_image(flatten=FALSE)` — flatten=true composites onto an opaque
   background and discards alpha (the "red bands" bug) — then **restore visibility**.
3. **Crop + anchor math** — `scripts/gimp_place.py spec.json`. Measures each layer's
   opaque rect (**alpha-THRESHOLD bbox, never `-trim`** — stray sub-1%-alpha pixels
   defeat `-trim` and the art renders tiny/squashed), tight-crops, and computes
   placements on the shared canvas ruler. Order `elements` bottom→top (emitted
   `zorder` follows input order). Threshold ~50 for crisp art; ~10–20 to keep a soft
   glow.
4. **Import** — same `asset_textures_import` settings as contract §5.
5. **Wire the widget.** Default **Mode A (native-pixel WYSIWYG)**: point anchor +
   native offsets —
   `Anchors min==max==(0,0)`, `Offsets=(Left=canvas_x, Top=canvas_y, Right=native_w,
   Bottom=native_h)`, `Alignment=(0,0)` — the slot is exactly native-pixel sized, both
   axes scale by one uniform DPI factor, **the element cannot distort**, any canvas or
   screen aspect. Mode B (fraction anchor box, `Offsets=0`) locks the HUD to screen
   *fractions* instead — pixel-faithful **only when canvas aspect == screen aspect**,
   else everything stretches by `screen/canvas` (3:2 art on 16:9 ⇒ ×1.19 wide). Use B
   only for a deliberately screen-fractional HUD authored at the target aspect. Either
   way: one consistent mode for every element (mixing framings is exactly what
   destroys relative sizing), `Brush.DrawAs = Image` (never Box/9-slice), explicit
   `ZOrder` from GIMP stacking, respect existing `BindWidget` names (change slot +
   ZOrder only; re-parenting a C++-bound image is risky).
6. **Verify in-engine — the only real arbiter.** `pie_start` → poll `pie_get_state` →
   `editor_screenshot` → **always `pie_stop`** (release the lease). Zoom the region
   (ImageMagick crop/resize), compare to the GIMP composite, iterate one defect at a
   time.

---

## Foot-gun catalog (each one cost a debugging loop)

- **`flatten=false` on every export** — flatten=true bleeds an opaque background in;
  `-trim` then keys off that color and mis-crops.
- **Alpha-THRESHOLD crop, never `-trim`** (mockup mode).
- **`lod_group:"UI"` + `mip_gen:"NoMipmaps"` for HUD art** — or it streams in blurry
  on first open. The import tool supports these natively; set them at import, not as
  a post-fix.
- **Texture mip-0 is not CPU-readable at runtime** — hit-testing needs the sidecar,
  never `GetPlatformData()`.
- **Full-canvas overlays live at (0,0)** — moved off, the co-registered stack silently
  misaligns (exporter warns).
- **Unique widget names in the layer stack** — `ConstructWidget` name reuse silently
  collapses siblings into one Image.
- **Committed script, never ad-hoc export** (contract mode) — drift in
  filenames/sizes breaks the join key invisibly.
- **One consistent placement mode, never mixed** (mockup mode).
- **`ZOrder` from GIMP stacking; `DrawAs: Image`** — never add-order or default brush.
- **`sRGB:true` for painted/albedo art**; masks/normals would be false.
- **PIE blocks asset + widget mutation** — stop it first; it re-arms if the user plays.
- **Never `save_xcf`; always restore layer visibility** — the file is the artist's;
  contract-mode alpha normalization happens on the export *duplicate* only.
- **Don't trust `is_group`/`list_layers`** — walk the tree with `call_api`.
- **The skill carries presentation + sidecars only** — runtime behaviour (fill
  materials, hit-test consumers, state machines, `BindWidget` data) is the consumer's;
  it cannot be derived from GIMP.

## Boundaries

- **Moves existing layers; does not create art.** Art generation/repair: `/icon`;
  standalone geometry measurement: `/see` (feed its numbers here).
- **One composition per run** (one `.xcf` → one widget or one contract). Re-run for
  another.
- Contract mode delivers the grammar mapping, the committed exporter + config, the
  textures, the manifest/sidecars, and the layer-stack widget scaffold — the
  interaction state machine and hit-test consumer are the game's C++ (propose, don't
  invent).
