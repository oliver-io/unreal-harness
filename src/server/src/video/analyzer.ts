/**
 * Video analysis adapter — MP4 + stated expectation → timestamped divergences.
 *
 * This is the ONLY vendor-coupled piece of the PIE video pipeline (the capture
 * primitive is self-hosted and produces a plain MP4). The coupling is
 * quarantined behind {@link VideoAnalyzer} so the provider can be swapped by
 * config without touching the tools.
 *
 * Google path: Files API upload (recordings exceed the ~20 MB inline cap) →
 * poll until ACTIVE → `generateContent` with a video part + structured-output
 * schema, so we never parse prose.
 *
 * Two fps knobs, deliberately distinct:
 *   - capture fps (pie_record_start): source fidelity — the frames that EXIST.
 *   - analysisFps (here): how densely the model SAMPLES the upload (default 1).
 *     Cost ≈ 300 tokens per sampled second; sample as low as the question allows.
 */

import { GoogleGenAI, Type } from "@google/genai";
import { config } from "../config.ts";

export interface AnalyzeSpec {
  /** What SHOULD happen, in plain language. */
  expectedBehavior: string;
  /** Optional explicit checks, each judged individually. */
  criteria?: string[];
  /** Model sampling density in frames/second (≤ capture fps to be useful). */
  analysisFps?: number;
  /** Optional clip window within the video, in seconds. */
  clip?: { startS?: number; endS?: number };
  /** Model id override; defaults to config.videoModel. */
  model?: string;
}

export interface Divergence {
  t_start_s: number;
  t_end_s: number;
  expected: string;
  observed: string;
  severity: "low" | "medium" | "high";
  confidence: number;
}

export interface AnalyzeResult {
  verdict: "matches" | "diverged" | "inconclusive";
  summary: string;
  divergences: Divergence[];
  criteria_results?: { criterion: string; passed: boolean; note: string }[];
  model: string;
  sampled_fps: number;
}

export interface VideoAnalyzer {
  analyze(mp4Path: string, spec: AnalyzeSpec): Promise<AnalyzeResult>;
}

const RESPONSE_SCHEMA = {
  type: Type.OBJECT,
  properties: {
    verdict: {
      type: Type.STRING,
      enum: ["matches", "diverged", "inconclusive"],
      description: "Overall judgement of the video against the expected behaviour.",
    },
    summary: { type: Type.STRING, description: "One-paragraph plain-language summary." },
    divergences: {
      type: Type.ARRAY,
      items: {
        type: Type.OBJECT,
        properties: {
          t_start_s: { type: Type.NUMBER, description: "Divergence start, seconds from clip start." },
          t_end_s: { type: Type.NUMBER, description: "Divergence end, seconds from clip start." },
          expected: { type: Type.STRING },
          observed: { type: Type.STRING },
          severity: { type: Type.STRING, enum: ["low", "medium", "high"] },
          confidence: { type: Type.NUMBER, description: "0..1" },
        },
        required: ["t_start_s", "t_end_s", "expected", "observed", "severity", "confidence"],
      },
    },
    criteria_results: {
      type: Type.ARRAY,
      description: "One entry per explicit criterion, in the order given.",
      items: {
        type: Type.OBJECT,
        properties: {
          criterion: { type: Type.STRING },
          passed: { type: Type.BOOLEAN },
          note: { type: Type.STRING },
        },
        required: ["criterion", "passed", "note"],
      },
    },
  },
  required: ["verdict", "summary", "divergences"],
} as const;

const SYSTEM_PROMPT =
  "You are a meticulous game-QA observer. You watch a recording of a live " +
  "game session and judge it against a stated expected behaviour and optional " +
  "explicit criteria. Report only what is actually visible: cite timestamps " +
  "(seconds from the start of the clip you were given), describe expected vs " +
  "observed concretely, and grade severity by gameplay impact. If the video " +
  "quality, framing, or sampling rate genuinely prevents judgement, say " +
  "'inconclusive' rather than guessing. Genuine matches are a valid, useful " +
  "answer — do not invent divergences.";

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

export class GoogleGenAIAnalyzer implements VideoAnalyzer {
  constructor(private readonly apiKey: string) {}

  async analyze(mp4Path: string, spec: AnalyzeSpec): Promise<AnalyzeResult> {
    const model = spec.model?.trim() || config.videoModel;
    const fps = spec.analysisFps && spec.analysisFps > 0 ? spec.analysisFps : config.videoDefaultAnalysisFps;
    const ai = new GoogleGenAI({ apiKey: this.apiKey });

    // 1. Upload (Files API) and wait until the service has processed it.
    const uploaded = await ai.files.upload({
      file: mp4Path,
      config: { mimeType: "video/mp4" },
    });
    const fileName = uploaded.name ?? "";
    try {
      let file = uploaded;
      const deadline = Date.now() + config.videoUploadTimeoutMs;
      while (file.state === "PROCESSING") {
        if (Date.now() > deadline) {
          throw new Error(
            `Gemini file processing timed out after ${Math.round(config.videoUploadTimeoutMs / 1000)}s`,
          );
        }
        await sleep(2000);
        file = await ai.files.get({ name: fileName });
      }
      if (file.state !== "ACTIVE" || !file.uri) {
        throw new Error(`Gemini file upload failed (state=${file.state ?? "unknown"})`);
      }

      // 2. Judge with structured output.
      const videoMetadata: Record<string, unknown> = { fps };
      if (spec.clip?.startS !== undefined) videoMetadata.startOffset = `${spec.clip.startS}s`;
      if (spec.clip?.endS !== undefined) videoMetadata.endOffset = `${spec.clip.endS}s`;

      const criteria = spec.criteria ?? [];
      const userPrompt =
        `EXPECTED BEHAVIOUR:\n${spec.expectedBehavior}\n` +
        (criteria.length
          ? `\nEXPLICIT CRITERIA (judge each):\n${criteria.map((c, i) => `${i + 1}. ${c}`).join("\n")}\n`
          : "") +
        `\nWatch the video and report the verdict, a summary, and every timestamped ` +
        `divergence from the expected behaviour.`;

      const response = await ai.models.generateContent({
        model,
        contents: [
          {
            role: "user",
            parts: [
              {
                fileData: { fileUri: file.uri, mimeType: "video/mp4" },
                videoMetadata,
              },
              { text: userPrompt },
            ],
          },
        ],
        config: {
          systemInstruction: SYSTEM_PROMPT,
          responseMimeType: "application/json",
          responseSchema: RESPONSE_SCHEMA,
        },
      });

      const text = response.text;
      if (!text) {
        throw new Error("Gemini returned an empty response");
      }
      const parsed = JSON.parse(text) as Omit<AnalyzeResult, "model" | "sampled_fps">;
      return { ...parsed, model, sampled_fps: fps };
    } finally {
      // The upload is transient by design — never leave gameplay footage parked
      // in the provider's file store.
      if (fileName) {
        await ai.files.delete({ name: fileName }).catch(() => {});
      }
    }
  }
}

/** Provider registry — config.videoProvider selects the implementation. */
export function getVideoAnalyzer(): { analyzer?: VideoAnalyzer; error?: string } {
  if (config.videoProvider !== "google") {
    return { error: `Unknown video provider '${config.videoProvider}' (supported: google).` };
  }
  if (!config.geminiApiKey) {
    return {
      error:
        "No Gemini API key configured. Set GEMINI_API_KEY (or GOOGLE_STUDIO_API_KEY) " +
        "in the repo-root .env and restart the server.",
    };
  }
  return { analyzer: new GoogleGenAIAnalyzer(config.geminiApiKey) };
}
