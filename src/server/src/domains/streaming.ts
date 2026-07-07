/**
 * Domain: stream — Pixel Streaming 2 control (portable.dev#19 M2).
 *
 * Streams the level editor viewport to a browser via PS2's EMBEDDED signalling
 * server. All three tools forward 1:1 to the bridge (wire name == tool name ==
 * C++ handler key, `MCPStreamingCommands.cpp`). Streaming is deliberately NOT
 * PIE-blocked: `stream_start` sets AutoStreamPIE so a Play-In-Editor session
 * hands the stream to the PIE viewport automatically.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const streamStart = bridgeTool({
  name: "stream_start",
  domain: "stream",
  description:
    "Start Pixel Streaming the level editor viewport via the embedded signalling " +
    "server (viewers browse to http://127.0.0.1:<viewer_port>). ASYNC: success " +
    "means result.state=\"starting\" — the first launch may download the " +
    "signalling-server bundle (needs internet, can take minutes); poll " +
    "stream_status until active is true. Idempotent: if already streaming, " +
    "returns the CURRENT state/ports (stop first to change ports). Also enables " +
    "AutoStreamPIE, so the stream follows a PIE session. Fails with " +
    "feature_disabled when the PixelStreaming2 plugin is unavailable.",
  input: z.object({
    viewer_port: z
      .number()
      .int()
      .min(1)
      .max(65535)
      .default(8890)
      .describe("HTTP port the embedded signalling server serves viewers on. Default 8890."),
    streamer_port: z
      .number()
      .int()
      .min(1)
      .max(65535)
      .default(8888)
      .describe("WebSocket port the editor streamer connects to. Default 8888."),
  }),
  annotations: { idempotentHint: true },
});

const streamStop = bridgeTool({
  name: "stream_stop",
  domain: "stream",
  description:
    "Stop Pixel Streaming (editor streamer + embedded signalling server). " +
    "Idempotent: succeeds even when nothing is streaming " +
    "(result.was_streaming tells which case it was).",
  input: z.object({}),
  annotations: { idempotentHint: true },
});

const streamStatus = bridgeTool({
  name: "stream_status",
  domain: "stream",
  description:
    "Read Pixel Streaming state: {active, viewer_port, streamer_port, " +
    "streamers[]}. `streamers` lists the IDs of live streamers (the editor " +
    "viewport streams as \"Editor\"); ports are null when the PixelStreaming2 " +
    "modules are unavailable.",
  input: z.object({}),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

export const streamingTools: ToolDef[] = [streamStart, streamStop, streamStatus];
