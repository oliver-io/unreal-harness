---
name: progress-video
description: >
  Turn a project's accumulated QA screenshots (projects/<Game>/Saved/Screenshots/**) into a
  development-story video — a curated, narrative-ordered slideshow that accelerates through the
  debug era, eases out to linger on the polished shots, crossfades throughout, and ends by
  dissolving into a real gameplay clip. Covers the whole pipeline: mining the screenshot folder +
  project docs (GDD/PLAN/STATUS) for the story arc, agent-verifying every candidate frame (drop
  contentless frames, KEEP funny bugs — errors preceding a good version are the story), moving
  keepers into named narrative groups, and generating the ffmpeg xfade chain with a tunable pacing
  curve — plus optional composition layers: a 3-column strip of dev screen-recordings riding above
  the main pane (1x speed, sampled), a pan-out camera revealing labels (repo URL, attribution, cost,
  fading credits) in the side black space, and a dual-aspect render emitting 16:9 and phone-native
  9:16 cuts in one pass. Use when someone wants a "progress video", "dev timeline video", "montage
  of the build", "slideshow of the screenshots", "tell the story of development", or "make a video
  from Saved/Screenshots". Produces: docs/media/composites/NN_GROUP_i.png + NARRATIVE.md + the
  .mp4 cut(s).
user-invocable: true
---

# progress-video

Build a development-story video from the screenshots automated QA leaves behind in
`projects/<Game>/Saved/Screenshots/`. Validated end-to-end on TRONG (833 shots → 50 story
groups → `docs/media/trong-dev-story.mp4`).

**The premise:** every agent-driven build session screenshots constantly — baselines, debug
frames, before/afters, iteration loops, hero shots. Their **file mtimes are the development
chronology**, their filenames are half a caption, and the project docs supply the plot. That
folder IS the story of the game being built; this skill turns it into a watchable artifact.

## Phase 1 — Mine the material

1. **Inventory chronologically.** File timestamps order the story:
   ```bash
   find projects/<Game>/Saved/Screenshots -type f \
     -printf '%T@ %TY-%Tm-%Td_%TH:%TM %s %p\n' | sort -n > shots_by_time.txt
   ```
   Look in `Saved/Screenshots/WindowsEditor/` (editor/PIE captures) AND the `Screenshots/`
   root (RT dumps and other sidecars land there).
2. **Read the plot.** `GDD.md` (what the game is), `PLAN.md` (build log), `STATUS.md`
   (dated entries = the act structure). Map date ranges → development eras (acts).
3. **Draft ~30–50 story groups** from filenames + timestamps: each group is one beat
   (a feature landing, a bug hunt, an iteration loop, a look-dev pass).

## Phase 2 — Verify every candidate frame (do not skip)

Filenames lie. In the TRONG run: `final_mount.png` still showed the scale bug,
`trong_rider_giant.png` showed no giant, `hud_debug.png` had no HUD, and ~15 frames were
all-black or subject-free. **Fan out reader agents** (batches of ~30–40 images each) that
`Read` every candidate and return one line per file:
`filename | literal 10–25 word description | OK or BAD(reason)`.

Selection doctrine (owner-confirmed):
- **Drop** frames devoid of content: all-black viewports, empty editor grids, subject
  off-frame, unreadable crops.
- **KEEP funny failures** — giant mannequins, invisible bikes, bloom whiteouts, comically
  wrong materials, glitched iteration frames. **An error preceding a good version is the
  best material you have**; order groups broken → fixed.
- A dark frame with visible neon content is fine (the game IS dark) — "devoid" means no
  subject, not low-key lighting.

## Phase 3 — Curate into named groups

Move (don't copy) keepers to `docs/media/composites/` as `NN_GROUP-SLUG_i.png`:
zero-padded group number `NN` orders the acts/beats, `i` orders frames within a beat
(chronological / broken→fixed). 2–6 frames per group. Write a `NARRATIVE.md` index in the
same folder: act structure, one caption per group. Beware `GROUPS` as a bash variable name —
it's a shell builtin (user's group IDs) and assignments to it are **silently ignored**.

Repair pass: some captures carry trailing garbage after the PNG image data (UE RT dumps do
this) — ffmpeg drops the frame. Detect and fix in place:

```bash
for f in *.png; do ffmpeg -v error -i "$f" -f null - 2>&1 | grep -q . && echo "BAD: $f"; done
ffmpeg -y -err_detect ignore_err -i bad.png -frames:v 1 clean.png && mv clean.png bad.png
```

## Phase 4 — Probe the ending clip, then generate the video

Match the slideshow to the gameplay clip that ends the video:

```bash
ffprobe -v error -show_entries stream=codec_name,width,height,r_frame_rate,sample_rate,channels \
  -of default=noprint_wrappers=1 docs/assets/media/<clip>.mp4
```

(TRONG's clip: 1276×718 @30fps h264 + 48kHz stereo AAC → canvas 1280×720@30.)

### Pacing curve (the design)

Per-image hold times follow **accelerate → floor → ease-out linger**:

- Start at `START` (0.5s) and decay geometrically (`R` = 0.983) — the early proxy-era
  shots read deliberately, then the montage speeds up.
- Floor at `FLOOR` (0.09s) through the mid-development flicker.
- Over the last `TAIL` fraction (0.20) of slides, **smoothstep** back up to `LINGER`
  (0.6s) — the polished-era shots get their moment.
- Each crossfade is `0.35 × min(neighbor holds)`, floored at 2 frames — fades scale with
  pacing automatically (dreamy early/late, soft blinks in the fast middle).

### The generator

One awk block emits three artifacts: `INPUTS` (per-image `-loop 1 -t <hold+fade> -i <file>`
args), `FILTER` (the 165-input xfade chain as a `-filter_complex_script` file — the graph is
far too big for the ~32k Windows command line), and the total duration. Run it from inside
the composites folder (relative paths keep the arg list short):

```bash
ls *.png | sort | awk '
BEGIN { FMIN=2/30.0; START=0.5; R=0.983; FLOOR=0.09; TAILFRAC=0.20; LINGER=0.6 }
{ n++; file[n]=$0 }
END {
  tail = int(n*TAILFRAC + 0.5); m = n - tail; d = START
  for (i=1;i<=m;i++) { dur[i]=d; d*=R; if (d<FLOOR) d=FLOOR }
  base = dur[m]
  for (j=1;j<=tail;j++) { t=j/tail; s=t*t*(3-2*t); dur[m+j] = base + (LINGER-base)*s }
  for (i=1;i<n;i++) { f=0.35*(dur[i]<dur[i+1]?dur[i]:dur[i+1]); if (f<FMIN) f=FMIN; fade[i]=f }
  fade[n]=0
  for (i=1;i<=n;i++) printf "-loop 1 -t %.4f -i %s ", dur[i]+fade[i], file[i] > "INPUTS"
  for (i=1;i<=n;i++) printf "[%d:v]scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2:black,setsar=1,fps=30,format=yuv420p[v%d];\n", i-1, i > "FILTER"
  cum=0; prev="v1"
  for (i=1;i<n;i++) {
    cum+=dur[i]; out=(i==n-1)?"vout":sprintf("x%d",i)
    printf "[%s][v%d]xfade=transition=fade:duration=%.4f:offset=%.4f[%s];\n", prev, i+1, fade[i], cum, out > "FILTER"
    prev=out
  }
  printf "%.3f\n", cum+dur[n]
}' > TOTAL
sed -i '$ s/;$//' FILTER     # a trailing semicolon breaks -filter_complex_script
```

Key invariant: with each input lasting `hold+fade`, the xfade **offset for transition i is
simply the cumulative hold time through image i** — no fade bookkeeping in the offsets.

### Render + assemble

```bash
# 1) the crossfaded slideshow (video only)
ffmpeg -y $(cat INPUTS) -filter_complex_script FILTER -map "[vout]" \
  -c:v libx264 -crf 20 -preset medium slideshow_fade.mp4

# 2) dissolve into the gameplay clip; its audio arrives on the crossfade
OFF=$(awk '{printf "%.3f", $1-0.4}' TOTAL)
ffmpeg -y -i slideshow_fade.mp4 -i <clip>.mp4 \
  -f lavfi -t "$OFF" -i anullsrc=r=48000:cl=stereo -filter_complex \
  "[0:v]fps=30,settb=AVTB[v0];\
   [1:v]scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2:black,fps=30,format=yuv420p,settb=AVTB[v1];\
   [v0][v1]xfade=transition=fade:duration=0.4:offset=$OFF[v];\
   [2:a][1:a]concat=n=2:v=0:a=1[a]" \
  -map "[v]" -map "[a]" -c:v libx264 -crf 20 -preset medium -c:a aac -b:a 160k out.mp4
```

## Phase 5 — Layered composition (strip, pan-out, labels, dual aspect)

All optional, all validated in the field. Each layer composes onto the Phase-4 slideshow
(`slideshow_fade.mp4` = the "main pane") before the finale xfade.

### Dev-capture strip (3 columns above the main pane)

If the project also has **screen recordings** of dev sessions (`D:\videos\...`, OBS dumps,
phone captures), stack a 3-column strip of them above the main pane. Rules that looked right:

- Columns are `W/3` wide (round: 426+426+428 = 1280), height `S` = pane height (240 for 720p),
  16:9-padded per video (`scale=<w>:<S>:force_original_aspect_ratio=decrease,pad=...`).
- Play at **1× speed**; when a video is longer than its slot, use a **leading sample**
  (`trim=duration=<slot>`). Budget each column to the slideshow length; distribute videos
  round-robin, but let the owner order them — hero captures open their columns at t=0,
  closers go last.
- Make each column slightly LONGER than the slideshow, then `hstack=inputs=3:shortest=1`
  and `vstack=inputs=2:shortest=1` so the main pane's length wins.
- (Variant, superseded by the strip: a corner PiP reel — each video time-compressed into an
  equal slot via `setpts=PTS*F,fps=30,trim,setpts=PTS-STARTPTS`, concat, then one
  `overlay=12:12:eof_action=pass`.)

### Pan-out (zoompan window over a wide canvas)

The tall block (strip + main = `W×(H+S)`, e.g. 1280×960) gets padded onto a 16:9 canvas
`W2 = even(ceil((H+S)*16/9))` (1708×960) with the block centered (`pad=W2:H2:(W2-W)/2:0`).
The camera is a zoompan window: **tight on the main pane at first, easing out to the wide
view, easing back in just before the finale**:

- `Z0 = W2/W` (1.334) — at `z=Z0` the window is exactly the main pane; at `z=1` the whole canvas.
- Window pinned bottom-center: `x='(iw-iw/zoom)/2'`, `y='ih-ih/zoom'` — the main pane never
  moves; the strip and side columns are what gets revealed.
- z-expression = smoothstep in/out via expression registers (`on` = output frame @30fps;
  hold 1s, out over 2.5s, back in ending ~0.2s before the xfade offset):
  ```
  z='st(1,min(max((on-30)/75,0),1));st(1,ld(1)*ld(1)*(3-2*ld(1)));
     st(2,min(max((on-F3)/60,0),1));st(2,ld(2)*ld(2)*(3-2*ld(2)));
     Z0-(Z0-1)*ld(1)+(Z0-1)*ld(2)'
  ```
  with `F3 = (xfade_offset - 2.2) * 30`. End with `d=1:fps=30:s=<W>x<H>`, then
  `format=yuv420p,settb=AVTB`.

### Labels in the revealed black space

`drawtext` onto the wide canvas BEFORE zoompan — anything drawn in the side columns is
naturally invisible while the window is tight and slides in with the pan:

- Side columns are narrow (`(W2-W)/2` ≈ 214px): center per line with `x='(214-text_w)/2'`
  (left) / `x='<W2-214>+(214-text_w)/2'` (right). **Break long strings into stacked lines**
  (a URL becomes `github.com/` / `owner/` / `repo`) and shrink any line that still overflows
  — spill past `W2` gets clipped by the frame edge.
- Timed fade-in (e.g. credits at the halfway mark): `fontcolor=white` +
  `alpha='0.9*min(max((t-19)/1.5\,0)\,1)'` — **commas inside the expression must be escaped
  `\,`** or the filter parser splits on them.
- Typical furniture: repo URL + model/attribution on one side; cost/provenance on the other;
  a "made with" credit block fading in later. Windows fontfile syntax:
  `fontfile='C\:/Windows/Fonts/arialbd.ttf'`.

### Dual-aspect output (16:9 + 9:16 in ONE pass)

Emit the desktop cut and a phone-native vertical cut from the same graph — `split` the tall
block and every reused input (`[1:v]split=2`, `[1:a]asplit=2`, `[2:a]asplit=2`; **a filter
stream can only be consumed once**), then give ffmpeg two `-map`/output groups.

Vertical (1080×1920): scale the tall block full-width (`1080×810`), pad into the middle
(`pad=1080:1920:0:(1920-810)/2`), labels move to the top/bottom BANDS (URL + attribution up
top, cost below the panes, credits bottom-right). **No zoom on 9:16** — any `z>1` window on a
full-width 16:9 pane crops it horizontally; the vertical cut holds the static stack and
xfades into the letterboxed finale. On a phone the vertical cut reads bigger than the 16:9
pan-out — post that one to vertical-first platforms.

## Phase 6 — Verify the output (frames, not vibes)

Extract frame pairs and `Read` them: two frames `START` apart near t=0 must be **adjacent**
slides; the same gap mid-video must span **several** slides; in the tail it must be adjacent
again; one frame mid-crossfade must show an actual blend; one frame after `OFF` must be the
gameplay clip. Downscale extracts to keep them cheap:

```bash
ffmpeg -y -ss <t> -i out.mp4 -frames:v 1 -vf scale=480:270 chk_<t>.png
```

## Foot-guns (all hit in the field)

| Trap | Fix |
|---|---|
| Windows ffmpeg treats `/c/Users/...` as `C:/c/Users/...` | `cygpath -m` every path that reaches ffmpeg |
| `xfade` "timebase do not match" | end **every** branch with `settb=AVTB` **after** `fps` (fps resets tb to 1/rate) |
| Trailing `;` in filter script | strip it — parse error otherwise |
| RT-dump PNGs with trailing garbage | re-encode with `-err_detect ignore_err` (Phase 3) |
| `GROUPS=` in bash silently no-ops | it's a shell builtin; use another name |
| 32k command-line limit | filtergraph goes in a `-filter_complex_script` file; run from the images dir for short relative input paths |
| concat-demuxer durations (naive variant) | last file must be repeated once with no `duration` line |
| a filter stream consumed twice → "Invalid argument" | `split`/`asplit` every stream feeding two branches (dual-output graphs especially) |
| commas inside `alpha=`/`enable=` expressions | escape as `\,` or the filtergraph parser splits the filter args |
| label text wider than its column | stack lines, shrink the long line, or clamp x — overflow past the canvas edge is silently clipped |
| `vstack`/`hstack` input length mismatch | default repeats last frame at EOF; overshoot columns slightly + `shortest=1` so the main pane decides length |
| zoompan on 9:16 output | any zoom crops a full-width 16:9 pane horizontally — vertical cuts stay static |
| labels clipped mid-pan | expected: text drawn outside the tight window slides in with the reveal; use alpha fade-in if that reads badly |

## Variants & extensions

- **Naive hard-cut variant** (fast preview): concat demuxer with `file`/`duration` pairs —
  no xfade chain, single `-vf scale,pad,fps` pass.
- **Effects pass:** per-group `transition=` (wipes/dips at act boundaries), title cards
  generated from `NARRATIVE.md` captions, Ken Burns via `zoompan` per still, a music bed
  that ducks under the clip's audio (`sidechaincompress` or keyframed `volume`).
- The whole generator is a candidate for a durable Bun script (`scripts/progress-video.ts`)
  if this graduates from recipe to tool — knobs above become CLI flags.

## Session hygiene

`INPUTS`/`FILTER`/`TOTAL`/`slides.txt` and frame-check PNGs are scratch — build them in the
session scratchpad, never commit them. The composites folder + `NARRATIVE.md` + the final
.mp4 are the deliverables. Mind repo size: a couple hundred full-res PNGs ≈ 250 MB+ —
consider release assets or a downscale pass before committing (see the
`github-readme-video-embedding` learnings for how GitHub handles big media).
