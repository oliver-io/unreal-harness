/**
 * Domain: video — analyze a recorded MP4 against a stated expectation.
 *
 * The generic analysis primitive of the PIE video pipeline. Input any MP4 (a
 * pie_record_stop artifact, or a human-provided clip) plus the behaviour it is
 * SUPPOSED to show; a video-understanding model returns a structured verdict
 * with timestamped, severity-rated divergences. Pure server-side — the editor
 * bridge is never involved and never sees the API key.
 */

import { z } from "zod";
import type { ToolDef, ToolContext } from "../registry/types.ts";
import { defineTool } from "./_shared.ts";
import { config } from "../config.ts";
import { getVideoAnalyzer, type AnalyzeSpec } from "../video/analyzer.ts";

export const videoAnalyzeInput = z.object({
  path: z
    .string()
    .describe(
      "Absolute path to the video file (e.g. result.path from pie_record_stop). MP4/H.264 expected.",
    ),
  expected_behavior: z
    .string()
    .describe(
      'What SHOULD happen in the clip, in plain language (e.g. "the ball bounces off the top wall and accelerates").',
    ),
  criteria: z
    .array(z.string())
    .default([])
    .describe("Optional explicit checks; each is judged individually (criteria_results)."),
  analysis_fps: z
    .number()
    .default(0)
    .describe(
      "How densely the MODEL samples the video, frames/second (default from config, " +
        "normally 1). Raise toward the capture fps only for fast/transient events — " +
        "cost is ~300 tokens per sampled second.",
    ),
  clip_start_s: z
    .number()
    .optional()
    .describe("Analyze only from this offset (seconds) into the video."),
  clip_end_s: z.number().optional().describe("Analyze only up to this offset (seconds)."),
  model: z
    .string()
    .default("")
    .describe("Model id override; defaults to the server's configured video model."),
});

/** Shared implementation for video_analyze and the pie_analyze composite. */
export async function analyzeVideoFile(
  args: z.infer<typeof videoAnalyzeInput>,
): Promise<Record<string, unknown>> {
  const { analyzer, error } = getVideoAnalyzer();
  if (!analyzer) {
    return { status: "error", error_code: "feature_disabled", error };
  }
  const file = Bun.file(args.path);
  if (!(await file.exists())) {
    return {
      status: "error",
      error_code: "invalid_path",
      error: `Video file not found: ${args.path}`,
    };
  }
  const fps = args.analysis_fps > 0 ? args.analysis_fps : config.videoDefaultAnalysisFps;
  if (fps > config.videoMaxAnalysisFps) {
    return {
      status: "error",
      error_code: "invalid_argument",
      error:
        `analysis_fps ${fps} exceeds the configured cost guard ` +
        `(${config.videoMaxAnalysisFps}). Sample lower, or clip with ` +
        `clip_start_s/clip_end_s and raise fps only over the window that matters.`,
    };
  }

  const spec: AnalyzeSpec = {
    expectedBehavior: args.expected_behavior,
    criteria: args.criteria,
    analysisFps: fps,
    model: args.model || undefined,
  };
  if (args.clip_start_s !== undefined || args.clip_end_s !== undefined) {
    spec.clip = { startS: args.clip_start_s, endS: args.clip_end_s };
  }

  try {
    const result = await analyzer.analyze(args.path, spec);
    return { status: "success", result: { ...result, path: args.path } };
  } catch (err) {
    return {
      status: "error",
      error_code: "internal",
      error: `Video analysis failed: ${err instanceof Error ? err.message : String(err)}`,
      result: { path: args.path, verdict: "inconclusive" },
    };
  }
}

const videoAnalyze = defineTool({
  name: "video_analyze",
  domain: "video",
  description:
    "Judge a recorded video (an MP4 — typically from pie_record_stop) against a " +
    "stated expected behaviour using a video-understanding model. Returns a " +
    "structured verdict ('matches'/'diverged'/'inconclusive'), a summary, " +
    "timestamped severity-rated divergences, and per-criterion results. The " +
    "upload to the provider is transient (deleted after analysis); the local " +
    "file is untouched. Two fps knobs exist: the recording's capture fps decides " +
    "which frames EXIST; analysis_fps decides how densely the model SAMPLES " +
    "them (~300 tokens per sampled second — default 1, raise only for fast/" +
    "transient events). Requires GEMINI_API_KEY (refused with feature_disabled " +
    "otherwise). Expect tens of seconds of latency (upload + inference).",
  input: videoAnalyzeInput,
  annotations: { readOnlyHint: true },
  handler: (args, _ctx: ToolContext) => analyzeVideoFile(args),
});

export const videoTools: ToolDef[] = [videoAnalyze];
