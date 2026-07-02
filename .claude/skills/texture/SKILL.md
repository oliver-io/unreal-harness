---
name: texture
description: Generate a tileable PBR texture set with local Python (numpy/PIL) AND land it as real Unreal assets over the MCP. Pass the full Python source as the argument — the skill stages and runs it, then imports the resulting PNGs to /Game/Generated/<slug>/, builds (or reuses) a master material, creates a per-slug Material Instance, and produces TestSphere + TestCube preview meshes wired to that MI. Use for procedural PBR sets (stone/metal/fabric/…), mesh-bake helpers, or any local Python that ends with files under assets/textures/. The decisive reason to bake a texture instead of computing noise live in a material: a baked texture has a MIP CHAIN, so it filters cleanly at distance/grazing/top-down — live shader noise has no mips and aliases into shimmer/crawl.
user-invocable: true
allowed-tools:
  - Bash
  - Write
  - Read
  - mcp__unrealMCP__asset_textures_import
  - mcp__unrealMCP__material_create
  - mcp__unrealMCP__material_create_instance
  - mcp__unrealMCP__material_add_expression
  - mcp__unrealMCP__material_set_expression_property
  - mcp__unrealMCP__material_connect
  - mcp__unrealMCP__material_compile
  - mcp__unrealMCP__material_instance_set_parameter
  - mcp__unrealMCP__material_read
  - mcp__unrealMCP__material_read_instance
  - mcp__unrealMCP__asset_list
  - mcp__unrealMCP__asset_duplicate
  - mcp__unrealMCP__mesh_set_static_mesh_material
  - mcp__unrealMCP__asset_save
  - mcp__unrealMCP__asset_open
  - mcp__unrealMCP__editor_screenshot
---

# texture — Python → tileable PBR set → real Unreal assets

A **two-stage pipeline** that replaces one-off `gen_*.py` texture scripts:

1. **Generate** — run the caller's Python locally to produce
   `assets/textures/<slug>/{basecolor,normal,orm,height,roughness,metallic,ao,tile_preview}.png`.
2. **Import** — drive the MCP to land those PNGs as real `UTexture2D` assets at
   `/Game/Generated/<slug>/`, build a per-slug `MI_<slug>` from the shared `_M_PBR_Master`,
   and produce per-slug `TestSphere` + `TestCube` static-mesh assets so the caller can verify the
   look on smooth and edged geometry.

> **Why bake instead of computing noise live in a material?** A baked texture has a **mip chain** —
> UE samples the right mip per screen pixel, so the surface filters smoothly and stays stable at any
> distance/angle. Procedural noise evaluated per-pixel in a material has **no mips**: at grazing /
> top-down / far angles, neighbouring pixels sample uncorrelated noise → aliasing → shimmer
> ("wet-lake ripple"), crawling cell-webs, and "blurry grey mud." If a live-material texture attempt
> is shimmering or webbing, that is the mip problem and **this skill is the fix** — not more tuning.

A texture run leaves these on disk:

```
assets/textures/<slug>/                  ← raw PNGs (source of truth, regenerable)
  basecolor.png  height.png  normal.png  roughness.png
  metallic.png   ao.png      orm.png     tile_preview.png

/Game/Generated/<slug>/                  ← imported UE assets
  basecolor   normal   orm   height       (UTexture2D, settings applied)
  roughness   metallic ao    tile_preview
  MI_<slug>                               ← Material Instance (parent: _M_PBR_Master)
  TestSphere                              ← UStaticMesh, slot 0 = MI_<slug>
  TestCube                                ← UStaticMesh, slot 0 = MI_<slug>
```

For a **game under `projects/<Name>/`**, prefer writing the PNGs under
`projects/<Name>/assets/textures/<slug>/` and importing to the same `/Game/Generated/<slug>/`
(the game's content root). The defaults below assume the harness root; adjust `ABS` accordingly.

## Argument

`$ARGUMENTS` — the **full Python source** to execute. Self-contained: imports, constants, `main()`,
and the `if __name__ == "__main__":` guard if needed.

## Runtime note — Python here is a deliberate exception to the Bun-first rule

This harness prefers Bun for tooling, but **tileable texture synthesis genuinely needs numpy + PIL**
(FFT bandpass noise, periodic blur, vectorised compositing) — there is no Bun-native equivalent, so
Python is the right runtime for the *generation* stage. The *import* stage is pure MCP. Don't port the
numpy math to JS.

## Execution — stage 1: run the Python

1. Pick a short kebab-case **slug** from the script's purpose — e.g. `weathered-slate`,
   `cobblestone-pbr`, `iron-roughness`. It becomes the staged filename, the output dir
   `assets/textures/<slug>/`, **and** the UE folder `/Game/Generated/<slug>/`.
2. Ensure `tmp/texture/` exists.
3. Write `$ARGUMENTS` to `tmp/texture/<slug>.py` via `Write`.
4. Run `python tmp/texture/<slug>.py` via `Bash`. Capture stdout + stderr. If `python` isn't on PATH,
   fall back to `/c/Python311/python` (this machine has numpy + Pillow there).
5. If the run failed, echo the traceback verbatim and **stop** — don't proceed to UE import.
   If a required package is missing, **stop and report** — do not `pip install` without the user's say-so.

The staged `.py` lives under `tmp/` (gitignored, session scratch). The script *output* (PNGs) is the
artifact that matters — per the harness rule, **delete the staged generator when done**; the PNGs +
imported assets are the source of truth, not the script.

## Execution — stage 2: import into Unreal

Skip this stage if the editor is unreachable (any MCP call returns engine-unavailable) — say so and stop.
All of stage 2 is **blocked during PIE** (asset mutators) — stop PIE first.

### 2a. Import the 8 PNGs — one `asset_textures_import` call

Per-channel sRGB and compression matter — wrong settings produce silently bad materials at runtime.
**Order matters: `normal` must appear before `orm` and `roughness`**, because both reference the imported
normal asset as their `composite_texture` for normal-aware roughness mips (Toksvig — keeps distant
surfaces from losing micro-roughness response down the mip chain).

```jsonc
// asset_textures_import
{
  "destination_folder": "/Game/Generated/<slug>",
  "force_overwrite": true,
  "images": [
    {"path": "<ABS>/basecolor.png",    "name": "basecolor",
     "settings": {"sRGB": true,  "compression": "Default"}},
    {"path": "<ABS>/normal.png",       "name": "normal",
     "settings": {"sRGB": false, "compression": "NormalMap"}},
    {"path": "<ABS>/orm.png",          "name": "orm",
     "settings": {"sRGB": false, "compression": "Masks",
                  "composite_texture": "/Game/Generated/<slug>/normal",
                  "composite_mode": "NormalRoughnessToGreen"}},
    {"path": "<ABS>/height.png",       "name": "height",
     "settings": {"sRGB": false, "compression": "Displacementmap"}},
    {"path": "<ABS>/roughness.png",    "name": "roughness",
     "settings": {"sRGB": false, "compression": "Grayscale",
                  "composite_texture": "/Game/Generated/<slug>/normal",
                  "composite_mode": "NormalRoughnessToRed"}},
    {"path": "<ABS>/metallic.png",     "name": "metallic",
     "settings": {"sRGB": false, "compression": "Grayscale"}},
    {"path": "<ABS>/ao.png",           "name": "ao",
     "settings": {"sRGB": false, "compression": "Grayscale"}},
    {"path": "<ABS>/tile_preview.png", "name": "tile_preview",
     "settings": {"sRGB": true,  "compression": "Default"}}
  ]
}
```

`<ABS>` is the absolute filesystem path to `assets/textures/<slug>/` (forward slashes are fine on Windows).

**Why ORM uses `NormalRoughnessToGreen` and roughness uses `NormalRoughnessToRed`:** ORM is RGB-packed
R=AO, G=Roughness, B=Metallic, so the roughness variance lives in G. The standalone `roughness.png` is
grayscale stored as R, so its variance lives in R. The wrong channel is silently wrong — the mips just
don't get the Toksvig adjustment.

### 2b. Ensure `/Game/Generated/_M_PBR_Master` exists

`asset_list(directory_path="/Game/Generated", class_filter="Material", recursive=false)` — if
`_M_PBR_Master` is absent, build it:

1. `material_create(material_path="/Game/Generated/_M_PBR_Master", shading_model="DefaultLit")`
2. Add four `TextureSampleParameter2D` nodes via `material_add_expression`
   (`expression_type="TextureSampleParameter2D"`, `parameter_name` = `BaseColor` / `Normal` / `ORM` /
   `Height`). The `Normal` sampler must sample as a normal map and `ORM`/`Height` as linear — the
   imported compression (NormalMap / Masks / Displacementmap) drives this; if a sample still reads as
   sRGB colour, set its `SamplerType` via `material_set_expression_property`
   (`SAMPLERTYPE_Normal` / `SAMPLERTYPE_LinearColor`).
3. Wire via `material_connect` (target_expression="Material"):
   - `BaseColor` RGB → `target_input="BaseColor"`
   - `Normal` RGB → `target_input="Normal"`
   - `ORM` → `target_input="AmbientOcclusion"` (source_output_index = R), `"Roughness"` (G), `"Metallic"` (B)
     — use a `ComponentMask` per channel if the tool needs single-float inputs.
4. `material_compile("/Game/Generated/_M_PBR_Master")`
5. `asset_save(["/Game/Generated/_M_PBR_Master"])`

The master has no default textures — it's a parameter shell; per-slug MIs supply the maps.

### 2c. Create the per-slug Material Instance

```jsonc
// material_create_instance
{"asset_path": "/Game/Generated/<slug>/MI_<slug>",
 "parent_material": "/Game/Generated/_M_PBR_Master", "force_overwrite": true}
```
Then bind the four texture parameters (one `material_instance_set_parameter` each,
`parameter_type="texture"`):
`BaseColor`→`basecolor`, `Normal`→`normal`, `ORM`→`orm`, `Height`→`height`
(`texture_path="/Game/Generated/<slug>/<tex>"`).

If a parameter name doesn't bind, run `material_read("/Game/Generated/_M_PBR_Master")` to confirm the
exact parameter names first.

### 2d. TestSphere + TestCube

```jsonc
// asset_duplicate ×2, then mesh_set_static_mesh_material ×2
{"source_path": "/Engine/BasicShapes/Sphere", "destination_path": "/Game/Generated/<slug>/TestSphere"}
{"source_path": "/Engine/BasicShapes/Cube",   "destination_path": "/Game/Generated/<slug>/TestCube"}
// mesh_set_static_mesh_material(mesh_path=".../TestSphere", material_path=".../MI_<slug>", slot_index=0)
// mesh_set_static_mesh_material(mesh_path=".../TestCube",   material_path=".../MI_<slug>", slot_index=0)
```
Then `asset_save([])` (save all dirty). The content-browser thumbnails show the material on smooth and
edged geometry — that's the per-slug confirmation; no screenshot needed for sanity.

### 2e. Optional — viewport preview

If the caller wants a preview image, `asset_open("/Game/Generated/<slug>/TestSphere")` then
`editor_screenshot(filename="<slug>_sphere")`. Compose only when asked.

### Integrating a baked set into an EXISTING hand-authored material

If the target isn't a fresh MI but an existing material with custom logic (e.g. a floor whose grid /
emissive lines live in the same material), **don't replace the material** — sample the baked maps into
it. Add `TextureSample` nodes for `basecolor`/`normal`/`orm`, drive their UVs from a world-aligned
coordinate (a `Custom`/`WorldPosition` XY × a tiling scalar — meshes like a big floor plane usually
have no usable UVs), and wire them into BaseColor / Normal / Roughness while leaving the existing
emissive untouched. World-aligned UVs + the texture's mips give stable, real surface detail at the very
top-down/grazing angles where live procedural noise failed.

## PBR physics — defaults that are hard to override accidentally

The maps model real surface optics. Most "this looks weird" results are an unphysical combination, not
a tuning miss. Honour these unless you have a specific, written reason.

### Metallic is binary
No half-metals in nature: a surface is a conductor (`metallic=1`) or a dielectric (`metallic=0`);
intermediate values are only legitimate as 1-pixel AA between the two. **Default `metallic=0`** for
weathered, painted, dirty, dusty, oxidized, stone, wood, ceramic, fabric, plastic, skin, dirt, water.
Reserve `metallic=1` for clean, polished, recently-machined metal. A rusted/painted/scratched metal
object is a **dielectric** (you're rendering the oxide/paint, an insulator). Convey wear through
**colour and roughness**, never a metallic blip.

### Roughness floor 0.55
Real weathered surfaces sit 0.65–0.92. Below 0.55 is shop-fresh polished steel, glass, or wet ceramic.
Default range **0.7–0.85** unless the subject is one of those three. (A *wet polished stone floor* is a
legitimate low-roughness case — but author it as a deliberate low value with variation, not a flat 0.05
mirror.)

### Normal-strength default 1.0
The pathological failure: dense small features at high strength → near-90° normal tilts at edges →
black cavities with bright Fresnel rims at glancing light. Fix is **always** lower strength / shallower
height — never metallic patches. Add "felt" via height depth, not strength.

### AO multiplies indirect, not direct
AO darkens indirect (skylight/environment) at cavities; it is **not** a darker-basecolor substitute.
Don't double-darken.

### The Fresnel trap
At glancing angles every surface gets a specular boost. Metallic patches embedded in a dielectric will
Fresnel-pop bright while the dielectric stays dim — reading as "wet sheen with deep cavities." That's a
metallic/dielectric contrast bug; make the whole surface dielectric.

### Variation comes from colour and roughness
When sketching a material, ask what changes across the surface — the answer should be expressible in
colour and roughness, not metallic.

## House conventions for PBR texture sets

**Output layout** — `assets/textures/<slug>/`:

| File | Format | Color space | Notes |
|------|--------|-------------|-------|
| `basecolor.png` | RGB uint8 | sRGB | UE linearizes on import (sRGB tag) |
| `height.png` | uint16 grayscale | linear | 16-bit displacement |
| `normal.png` | RGB uint8 | linear | Tangent-space, **DirectX/Unreal — green inverted** |
| `roughness.png` | uint8 grayscale | linear | |
| `metallic.png` | uint8 grayscale | linear | all zeros for dielectrics |
| `ao.png` | uint8 grayscale | linear | |
| `orm.png` | RGB uint8 | linear | Packed **R=AO, G=Roughness, B=Metallic** |
| `tile_preview.png` | RGB uint8 | sRGB | 2×2 tiled basecolor at half size — seam QA |

**Default size** 2048². **Use a fixed seed** so reruns are deterministic. Accept a `--size`
(`1024|2048|4096`) flag. Don't emit multiple resolutions — UE's mip chain *is* the LOD system.

### Frequency vs resolution — the trap
**A 4k image with 2k of content is just an upscaled 2k texture.** To make 4k actually be 4k:

| Parameter type | Scaling rule | Reason |
|----------------|--------------|--------|
| Density of stamped features (pit/scratch counts) | × `(SIZE/2048)²` | counts are per-area |
| Pixel-size measurements (sigmas, lengths) | × `(SIZE/2048)` | lengths are per-edge |
| fbm octaves | + `log2(SIZE/2048)` | each octave doubles frequency |
| Voronoi cell count, fbm base_freq, [0,1] edge widths | unchanged | image-relative macro layout |

```python
SIZE_SCALE    = SIZE / 2048.0
DENSITY_SCALE = SIZE_SCALE * SIZE_SCALE
PIXEL_SCALE   = SIZE_SCALE
EXTRA_OCTAVES = int(round(math.log2(SIZE_SCALE))) if SIZE_SCALE != 1.0 else 0
```

### Materials are records, not noise — multi-process composition
**A real surface is the cumulative record of many independent processes, each at a different scale and
statistical signature.** One fbm + one Voronoi + one stamp field is "noise dressed up as a material" —
the eye, trained on real surfaces all its life, can tell instantly. Move from "noise-with-colour" to
"surface-with-history" by stacking *structurally different* layers, not by tuning one layer harder. For
a weathered stone floor the processes that should be present:

| Process | Scale (cycles/img) | Primitive | Adds |
|---------|--------------------|-----------|------|
| Mineral/crystal grain | sub-pixel ↔ 64+ | high-freq fbm | pixel-scale variation ("4k of detail" vs "4k pixels") |
| Broad tonal weathering | 2–6 | low-freq fbm | macro light/dark zones |
| Mineral phase / staining | 4–16 | independent low-freq fbm | the multi-tone look real stone has |
| Slab / patch layout | 6–20 | Voronoi cell-fill | macro flagstone regions (if applicable) |
| Joint / crack network | 16–32 | Voronoi cell-edge (torus dist) | the mid-scale structural band where "is this real?" is judged |
| Surface lumps / nodules | 30–80 | small-cell Voronoi | tactile relief in height |
| Pit fields w/ rims | sparse, multi-scale | stamp + blur, 2 sigmas | corrosion/erosion pits, bright raised rim + dark core |
| Pit-density modulation | 2–4 | low-freq fbm | clustered, not uniform Poisson |
| Mechanical wear / polish streaks | sparse oriented | scratch stamps | foot-traffic polish, tool marks |

**Energy-spectrum mental model:** imagine FFT'ing your basecolor. Real photos roll off smoothly across
all frequencies; poor synthetics have **gaps** — usually the **16–64 cycles/image** "fingertip-sized
features" band. Crackle networks and small-Voronoi fill it; without them you get the "blurry wash with
sprinkles" look that screams synthetic.

**Pit/erosion rims:** real pits aren't dark dots — corrosion/weathering product has more volume than
what it replaced and pushes outward, leaving a brighter raised ring. Render with two Gaussian sigmas
from the same source positions (rim first toward bright, then dark core on top). Dramatic realism gain
for ~40 lines.

### Structural features need coverage modulators
A crack network / stamp field applied **uniformly** reads as "this surface was tiled," not "this
surface has cracks." Real surfaces have regions where features appear and regions where they don't.
Multiply every structural pattern by an **independent** low-freq coverage field:

```python
crackle_raw      = tileable_voronoi_crackle(SIZE, num_cells=130, edge_width=0.0025, seed=11)
crackle_coverage = tileable_fbm(SIZE, base_freq=2.5, octaves=2+EXTRA_OCTAVES, persistence=0.5, seed=99)
crackle          = crackle_raw * (0.10 + 0.90 * crackle_coverage)   # seed 99 ≠ 11
```
If the coverage seed equals the structure seed they correlate → "every cell partially faded" instead
of "clusters present, others absent." Same rule for scratches, nodules, pit clusters.

### A high-frequency layer is not optional
Even with the scaling above, *coarse Voronoi + one mid fbm + sparse pits* gives blurry 4k. There must
be content at **all** scales including a near-pixel fine-grain fbm (finest wavelength ~4px regardless of
resolution). Modulate basecolor (±5%), roughness (±0.04), and a small height contribution (±0.005) with
it. **Shared-substrate principle:** every map (basecolor/height/normal/roughness/AO) reads from one set
of underlying noise fields, so a crack/dip/speck lands in the same pixels across all maps. Independent
noise per map looks disconnected at runtime.

## Tileable noise primitives (copy verbatim — periodic by construction)

```python
import numpy as np

def tileable_fbm(size, base_freq, octaves, persistence, seed):
    """Tileable fractal noise in [0,1] via FFT bandpass octaves."""
    rng = np.random.default_rng(seed)
    fx = np.fft.fftfreq(size) * size
    fy = np.fft.fftfreq(size) * size
    fx, fy = np.meshgrid(fx, fy, indexing="xy")
    freq = np.sqrt(fx * fx + fy * fy)
    weight = np.zeros_like(freq); amp = 1.0
    for o in range(octaves):
        center = base_freq * (2 ** o)
        sigma = max(center * 0.5, 1.0)
        weight += amp * np.exp(-0.5 * ((freq - center) / sigma) ** 2)
        amp *= persistence
    spectrum = (rng.standard_normal((size, size))
                + 1j * rng.standard_normal((size, size))) * weight
    spectrum[0, 0] = 0.0
    field = np.real(np.fft.ifft2(spectrum))
    return ((field - field.min()) / (field.max() - field.min() + 1e-12)).astype(np.float32)

def tileable_blur(arr, sigma_pixels):
    """Gaussian blur with periodic boundaries (FFT)."""
    size = arr.shape[0]
    fx = np.fft.fftfreq(size) * size
    fy = np.fft.fftfreq(size) * size
    fx, fy = np.meshgrid(fx, fy, indexing="xy")
    freq2 = fx * fx + fy * fy
    kernel = np.exp(-2 * (np.pi ** 2) * freq2 * (sigma_pixels ** 2) / (size ** 2))
    return np.real(np.fft.ifft2(np.fft.fft2(arr) * kernel)).astype(np.float32)

def sobel_normal_dx(height_norm, strength=4.0):
    """Tangent-space normal, DirectX (Unreal) — Y/green inverted. Periodic via np.roll."""
    h = height_norm * strength
    gx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * 0.5
    gy = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) * 0.5
    nx, ny, nz = -gx, -gy, np.ones_like(h)
    length = np.sqrt(nx * nx + ny * ny + nz * nz)
    nx /= length; ny /= length; nz /= length
    r = (nx + 1) * 0.5
    g = 1.0 - (ny + 1) * 0.5    # DirectX flip
    b = (nz + 1) * 0.5
    return np.clip(np.stack([r, g, b], axis=-1) * 255, 0, 255).astype(np.uint8)

def pack_orm(ao_u8, rough_u8, metal_u8):
    """Unreal standard packed: R=AO  G=Roughness  B=Metallic."""
    return np.stack([ao_u8, rough_u8, metal_u8], axis=-1)
```

**Voronoi crackle** (cell-edge networks): use **torus distance** (`dx = min(|dx|, 1-|dx|)`, same for dy)
so seams disappear; compute in row chunks to avoid an `(H,W,N)` allocation at full res.
**Stamp + blur** (pits, specks): scatter integer indices into a zero field via `np.add.at`, then
`tileable_blur` to make the result periodic.

## When NOT to use this skill
- **Image-gen via external APIs** (OpenAI etc.) — that's an icon/`/icon`-style path, not this. This skill
  is for *deterministic, local* numpy/PIL work.
- **Anything that touches the live editor at generation time** — Python here is a plain subprocess, not
  inside UE; use the MCP for editor mutation.
- **Scripts meant to be re-run by other tools/people** — promote them from `tmp/texture/` to `scripts/`
  or the project's `Tools/` and delete the staged copy (ask first). The staged generator is scratch.
