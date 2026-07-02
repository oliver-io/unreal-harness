#!/usr/bin/env bun
/**
 * visual-critique — one external critic for a RENDER against a DESIRED STATE that is EITHER
 * a reference IMAGE or a written TEXT spec. Two modes, one tool:
 *
 *   IMAGE mode  (--reference <img>):  art-direction comparison. Sends REFERENCE + RENDER to Gemini and
 *                                     returns a calibrated, rubric-scored breakdown of what matches and
 *                                     what differs, per element, with engine-specific fixes.
 *   SPEC  mode  (--desired "<text>"): adversarial spec-conformance. Decomposes the written desired state
 *                                     into independently-checkable attributes and, per attribute, reports
 *                                     DESIRED vs OBSERVED with a pass/fail verdict and a specific fix.
 *
 * Either a reference image OR a desired-state text is required (exactly one) — that choice selects the mode.
 * Both stances are GROUNDED: every cited difference must be genuinely visible; the critic never confabulates
 * a flaw or assumes failure, and it credits what is genuinely met. (An earlier "assume it's wrong, cap the
 * score" prompt produced contradictory criticism — do not reintroduce it.)
 *
 * Usage:
 *   bun .claude/skills/visual-critique/critique.ts --reference <img> --render <img> [options]
 *   bun .claude/skills/visual-critique/critique.ts --desired "<text>" --render <img> [options]
 *   bun .claude/skills/visual-critique/critique.ts --desired-file <path> --render <img> [options]
 * Options:
 *   --focus "<areas>"   Areas/attributes to scrutinize hardest (e.g. "floor material" / "color, centering").
 *   --context "<text>"  Scene ground truth; NOT penalized and NOT listed as a difference/violation.
 *   --model <id>        Default gemini-3.5-flash (falls back to gemini-3.1-pro-preview on 404/400).
 *   --json              Emit machine-readable JSON (schema differs per mode; see jsonInstr below).
 *   --gate <n>          Exit code 3 if the score < n (use to fail a loop until quality/conformance is reached).
 *
 * Key: GOOGLE_STUDIO_API_KEY (Bun auto-loads repo .env; a parent-dir .env walk is the fallback). Never printed.
 */
import { readFileSync, existsSync } from "node:fs";
import { resolve } from "node:path";

const argv = process.argv;
const arg = (n: string, fb = "") => {
  const i = argv.indexOf(`--${n}`);
  return i >= 0 && i + 1 < argv.length ? argv[i + 1] : fb;
};
const flag = (n: string) => argv.includes(`--${n}`);

function loadKey(): string {
  if (process.env.GOOGLE_STUDIO_API_KEY) return process.env.GOOGLE_STUDIO_API_KEY;
  let dir = process.cwd();
  for (let i = 0; i < 7; i++) {
    const p = resolve(dir, ".env");
    if (existsSync(p)) {
      const m = readFileSync(p, "utf8").match(/^\s*GOOGLE_STUDIO_API_KEY\s*=\s*(.+?)\s*$/m);
      if (m) return m[1].trim();
    }
    const up = resolve(dir, "..");
    if (up === dir) break;
    dir = up;
  }
  throw new Error("GOOGLE_STUDIO_API_KEY not found (env or .env)");
}

function imgPart(path: string) {
  const buf = readFileSync(path);
  const lower = path.toLowerCase();
  const mime = lower.endsWith(".jpg") || lower.endsWith(".jpeg") ? "image/jpeg" : "image/png";
  return { inline_data: { mime_type: mime, data: buf.toString("base64") } };
}

// --- Inputs. Exactly one of (reference image) | (desired text) selects the mode. ---
const referencePath = arg("reference") || arg("ref");
const desiredInline = arg("desired") || arg("spec");
const desiredFile = arg("desired-file");
const desired = desiredFile
  ? (existsSync(desiredFile) ? readFileSync(desiredFile, "utf8").trim() : "")
  : desiredInline;
const renderPath = arg("render");
const focus = arg("focus");
const context = arg("context");
const asJson = flag("json");
const gate = arg("gate") ? Number(arg("gate")) : null;
const model = arg("model", "gemini-3.5-flash");

const hasReference = !!referencePath;
const hasDesired = !!desired || !!desiredFile;

if (!renderPath || (!hasReference && !hasDesired)) {
  console.error(
    'Usage: --render <img> AND exactly one of:\n' +
      '  --reference <img>            (IMAGE mode: compare against a reference image)\n' +
      '  --desired "<text>" | --desired-file <path>   (SPEC mode: verify against a written desired state)\n' +
      'Options: [--focus "..."] [--context "..."] [--json] [--gate N] [--model id]',
  );
  process.exit(2);
}
if (hasReference && hasDesired) {
  console.error("Provide EITHER --reference (image target) OR --desired/--desired-file (text target), not both.");
  process.exit(2);
}
if (desiredFile && !desired) {
  console.error(`Desired-state file not found or empty: ${desiredFile}`);
  process.exit(2);
}
if (!existsSync(renderPath)) { console.error(`File not found: ${renderPath}`); process.exit(2); }
if (hasReference && !existsSync(referencePath)) { console.error(`File not found: ${referencePath}`); process.exit(2); }

const mode: "image" | "spec" = hasReference ? "image" : "spec";
const key = loadKey();

// ── IMAGE mode (render vs reference image): calibrated, honest art-direction comparison ──────────────
const IMAGE_RUBRIC = `SCORING RUBRIC — score what you ACTUALLY SEE, calibrated honestly (no artificial caps, no assuming failure):
- 90-100: Reads as the same shot — form, silhouette, color, value range, material response (gloss/reflection/emissive), composition all match.
- 70-89: Clearly the same scene; minor material/lighting/color gaps remain.
- 50-69: Same intent and structure; some real divergences in palette, contrast, materials, or placement.
- 30-49: Recognizable but with significant differences.
- 0-29: Wrong palette / crushed-or-blown values / genuinely flat-or-missing materials / wrong structures.
Pick the band the render genuinely sits in. Do NOT cap a score to "look strict". A render that matches well deserves a high score.`;

const IMAGE_TASK = `You are a principal art director AND a senior Unreal Engine technical artist comparing a real-time render against concept art.
IMAGE 1 = REFERENCE (concept art — the TARGET).
IMAGE 2 = RENDER (the in-engine screenshot trying to reproduce IMAGE 1).

Be RIGOROUS but ACCURATE — your value is correct, grounded observations, not performative harshness.
- Describe what is ACTUALLY in the render. Every difference you cite must be something genuinely visible in IMAGE 2 — do NOT invent flaws, and do NOT assume the render is wrong.
- Credit what genuinely matches as well as what differs; a fair review names both.
- If something is translucent, reflective, textured, or present, say so — do not default to "opaque/flat/missing/placeholder" unless that is what you truly see.
- State exact, observable facts (too dim/bright, wrong hue, flat vs textured, missing, occluded, wrong shape/scale/position) and ground each in the image.
- Every fix must be specific and engine-actionable (material node graph, roughness/metallic/emissive values, post-process like bloom/exposure/contrast, mesh/placement, reflection method).
${context ? `SCENE CONTEXT (treat as ground truth; do NOT penalize the score for any of these, and do NOT list them as failures): ${context}\n` : ""}${focus ? `FOCUS — judge primarily on: ${focus}\n` : ""}`;

const IMAGE_JSON_INSTR = `Return ONLY JSON, no markdown, matching:
{"match_score": <int 0-100>, "verdict": "<one accurate sentence>", "matches": ["<what genuinely matches the reference>"], "blocking_failures": ["<ranked real differences, most severe first>"], "elements": {"<element>": {"score": <int>, "issue": "<what actually differs vs reference, or 'matches' if it does>", "fix": "<engine-specific action>"}}, "prioritized_fixes": ["<most impactful first>"], "requires_for_90": ["<what a 90+ render must have>"]}
Cover whichever of these elements are present in the images: floor/ground material, key structures, foreground/midground/background objects, black level & contrast, bloom/glow, reflections, color palette — plus any other prominent element you see.`;

const IMAGE_TEXT_INSTR = `Respond in tight plain text:
1. MATCH SCORE: <0-100> — one line justifying it against the rubric.
2. BLOCKING FAILURES: ranked list, most severe first, one concrete line each.
3. PER ELEMENT (only those present): each as "<element>: <issue> -> <engine fix>".
4. PRIORITIZED FIXES: most impactful first.
5. REQUIRES FOR 90+: what this render must have to reach art-direction parity.`;

// ── SPEC mode (render vs written desired state): adversarial, grounded attribute conformance ─────────
const SPEC_RUBRIC = `SCORING — CONFORMANCE SCORE is the share of the desired attributes the render genuinely satisfies, weighted by how badly each miss matters:
- 90-100: Every stated attribute is clearly met; at most trivial, hard-to-see deviations remain.
- 70-89: All major attributes met; one or more MINOR attributes are off (a shade, a few px of offset).
- 50-69: Some real, plainly-visible attribute is violated (wrong-but-adjacent color, wrong proportion, slightly off-center).
- 30-49: A defining attribute is wrong (named the wrong color family, wrong shape, clearly mis-placed).
- 0-29: The result does not match the spec — multiple defining attributes wrong or the subject is absent.
Score the band the render genuinely sits in. A render that meets the spec deserves a high score; do NOT cap it to look strict.`;

const SPEC_TASK = `You are an adversarial QA reviewer and senior technical artist verifying a single RENDER against a written DESIRED STATE (a spec).
IMAGE = the RENDER (the actual result produced).
DESIRED STATE (the spec the render is SUPPOSED to satisfy):
"""
${desired}
"""

YOUR JOB is to find EVERY way the render deviates from the desired state, attribute by attribute.
1. Decompose the desired state into its individual, independently-checkable attributes. Treat each adjective, measurement, color, shape, position, count, and relationship as a SEPARATE attribute (e.g. "a crimson, square, centered button" = {color: crimson}, {shape: square}, {position: centered}, {subject: button}).
2. For EACH attribute, state what was DESIRED and what you ACTUALLY OBSERVE in the render, then give a verdict: pass | fail.
3. Be adversarial: a claimed attribute passes ONLY if the render clearly satisfies it. If it is wrong-but-close, say so precisely and mark it fail — name the SPECIFIC deviation: "desired crimson (deep blue-red) but observed bright/orange red", "desired square but observed a wide rectangle (~1.6:1)", "desired centered but observed shifted ~15% left".
4. Stay grounded — every fail you cite must be something genuinely visible in the render. Do NOT invent flaws, do NOT assume failure, and DO credit attributes that are genuinely met (verdict pass).
5. Each fix must be specific and actionable (exact color/hex direction, dimension/ratio change, offset direction & amount, material/post/mesh change).
${context ? `SCENE CONTEXT (ground truth — do NOT penalize the score for these and do NOT list them as violations): ${context}\n` : ""}${focus ? `FOCUS — scrutinize these attributes hardest: ${focus}\n` : ""}`;

const SPEC_JSON_INSTR = `Return ONLY JSON, no markdown, matching:
{"conformance_score": <int 0-100>, "verdict": "<one accurate sentence>", "attributes": [{"attribute": "<name e.g. color/shape/position/count/subject>", "desired": "<what the spec asked for>", "observed": "<what is actually in the render>", "verdict": "pass"|"fail", "severity": "trivial"|"minor"|"major"|"defining", "fix": "<specific action, or '' if pass>"}], "violations": ["<ranked fails, most severe first, each naming desired-vs-observed>"], "passes": ["<attributes genuinely satisfied>"], "requires_for_pass": ["<what must change for full conformance>"]}`;

const SPEC_TEXT_INSTR = `Respond in tight plain text:
1. CONFORMANCE SCORE: <0-100> — one line justifying it against the rubric.
2. VIOLATIONS: ranked, most severe first. Each: "<attribute>: desired <X> but observed <Y> -> <fix>".
3. ATTRIBUTE CHECK: one line per decomposed attribute — "<attribute>: desired <X> | observed <Y> | PASS/FAIL".
4. PASSES: attributes that genuinely match the spec.
5. REQUIRES FOR PASS: what must change for the render to fully satisfy the desired state.`;

const TASK = mode === "image" ? IMAGE_TASK : SPEC_TASK;
const RUBRIC = mode === "image" ? IMAGE_RUBRIC : SPEC_RUBRIC;
const jsonInstr = mode === "image" ? IMAGE_JSON_INSTR : SPEC_JSON_INSTR;
const textInstr = mode === "image" ? IMAGE_TEXT_INSTR : SPEC_TEXT_INSTR;

const prompt = `${TASK}\n${RUBRIC}\n\n${asJson ? jsonInstr : textInstr}`;
const parts =
  mode === "image"
    ? [{ text: prompt }, imgPart(referencePath), imgPart(renderPath)]
    : [{ text: prompt }, imgPart(renderPath)];

const url = (m: string) => `https://generativelanguage.googleapis.com/v1beta/models/${m}:generateContent`;

async function callGemini(m: string, p: any[], wantJson: boolean) {
  const generationConfig: any = { temperature: 0.2, maxOutputTokens: 8192 };
  if (wantJson) generationConfig.responseMimeType = "application/json";
  return fetch(url(m), {
    method: "POST",
    headers: { "content-type": "application/json", "x-goog-api-key": key },
    body: JSON.stringify({ contents: [{ parts: p }], generationConfig }),
  });
}

const base = (p: string) => p.split(/[\\/]/).pop();
let usedModel = model;
let res = await callGemini(model, parts, asJson);
if ((res.status === 404 || res.status === 400) && model !== "gemini-3.1-pro-preview") {
  console.error(`[model] ${model} returned HTTP ${res.status}; falling back to gemini-3.1-pro-preview`);
  usedModel = "gemini-3.1-pro-preview";
  res = await callGemini("gemini-3.1-pro-preview", parts, asJson);
}
const tag =
  mode === "image"
    ? `ref=${base(referencePath)} · render=${base(renderPath)}`
    : `render=${base(renderPath)} · desired="${desired.slice(0, 60).replace(/\s+/g, " ")}${desired.length > 60 ? "…" : ""}"`;
console.error(`[model] ${usedModel} (HTTP ${res.status}) · mode=${mode} · ${tag}`);
if (!res.ok) {
  console.error(`HTTP ${res.status}:\n${await res.text()}`);
  process.exit(1);
}
const json: any = await res.json();
const text = json?.candidates?.[0]?.content?.parts?.map((p: any) => p.text).filter(Boolean).join("") ?? "";
if (!text) { console.error("Empty response:\n" + JSON.stringify(json, null, 2)); process.exit(1); }
console.log(text);

// Gate: parse the mode's score key (JSON or plain text) and fail the process if below threshold.
if (gate != null) {
  const m =
    text.match(/"match_score"\s*:\s*(\d+)/) ||
    text.match(/MATCH SCORE:\s*(\d+)/i) ||
    text.match(/"conformance_score"\s*:\s*(\d+)/) ||
    text.match(/CONFORMANCE SCORE:\s*(\d+)/i);
  const score = m ? Number(m[1]) : NaN;
  if (!Number.isNaN(score)) {
    console.error(`\n[gate] score ${score} vs threshold ${gate}: ${score < gate ? "FAIL" : "PASS"}`);
    if (score < gate) process.exit(3);
  } else {
    console.error("[gate] could not parse score; treating as fail.");
    process.exit(3);
  }
}
