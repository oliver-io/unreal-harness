/**
 * Self-describing StreamSourceDescriptor surface, for an EXTERNAL local process
 * (the Portable backend) to learn how to reach and play THIS producer without a
 * static, per-repo config file:
 *
 *   GET /stream-source → a neutral StreamSourceDescriptor (see
 *     mobile-vgit/docs/STREAM-SOURCE-PROTOCOL.md).
 *
 * This REPLACES the deprecated static `portable.json` model: a producer now
 * describes itself at runtime over its own control port. Ports come from
 * config.ts (mcpPort / viewerPort) — never re-hardcode a value that has a
 * config source. Loopback-only, no auth by design — same posture as /status.
 */

import type { IncomingMessage, ServerResponse } from "node:http";
import { config } from "../config.ts";

/** Neutral descriptor of a single stream producer. */
export interface StreamSourceDescriptor {
  version: number;
  sourceId: string;
  sourceKind: string;
  title: string;
  icon: string;
  control: {
    baseUrl: string;
    status: string;
    start: string;
    stop: string;
    descriptor: string;
  };
  player: {
    type: string;
    signalling: string;
    viewerPort: number;
    urlParams: Record<string, string>;
  };
  input: {
    scheme: string;
    controls: string[];
  };
  meta: Record<string, unknown>;
}

/** Build the descriptor for this producer from runtime config. */
export function buildDescriptor(): StreamSourceDescriptor {
  return {
    version: 1,
    sourceId: config.streamSourceId || `source:${config.mcpPort}`,
    sourceKind: "pixelstreaming2",
    title: config.streamSourceTitle,
    icon: "gamepad",
    control: {
      baseUrl: `http://127.0.0.1:${config.mcpPort}`,
      status: "/status",
      start: "/control/stream/start",
      stop: "/control/stream/stop",
      descriptor: "/stream-source",
    },
    player: {
      type: "pixelstreaming2",
      signalling: "ss-query",
      viewerPort: config.viewerPort,
      urlParams: {
        StreamerId: "Editor",
        AutoConnect: "true",
        AutoPlayVideo: "true",
        StartVideoMuted: "true",
      },
    },
    input: {
      scheme: "ps2-uiinteraction",
      controls: ["look", "pan", "dolly", "orbit", "tap", "focus", "pie"],
    },
    meta: { engine: "UnrealEngine", pieCapable: true },
  };
}

function send(res: ServerResponse, code: number, body: unknown): void {
  res.writeHead(code, { "Content-Type": "application/json" });
  res.end(JSON.stringify(body));
}

/** Handle a /stream-source request. Returns true if it owned the request. */
export async function handleDescriptorHttp(
  req: IncomingMessage,
  res: ServerResponse,
): Promise<boolean> {
  const pathname = (req.url ?? "").split("?")[0];
  if (pathname !== "/stream-source") return false;

  if (req.method === "GET") {
    send(res, 200, buildDescriptor());
    return true;
  }

  send(res, 404, {
    ok: false,
    error: `no stream-source route for ${req.method} ${pathname}`,
  });
  return true;
}
