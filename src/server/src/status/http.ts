/**
 * Plain-HTTP liveness surface, for an EXTERNAL local process (e.g. the
 * Portable backend) to poll editor state without an MCP session:
 *
 *   GET /status → { editorUp, phase, pieActive, liveCodingInProgress,
 *                   project, lastProbeAt, stream? }
 *
 * `stream` ({active, viewerPort, streamers} — portable.dev#19 M2) is surfaced
 * only when the cached `mcp_status` reply carries the bridge's `stream` field
 * (a plugin build that predates it simply omits the key — every pre-existing
 * field is unchanged, so old pollers are unaffected).
 *
 * Read-only over the lifecycle watchdog's cached `mcp_status` probe
 * (`bridge/lifecycle.ts`) — it never probes the editor itself, so polling is
 * free and adds zero bridge traffic. Before the first watchdog probe (the
 * ~15s startup grace) it reports the zero-state (editorUp false,
 * lastProbeAt null) with HTTP 200 so pollers branch on fields, not errors.
 * Loopback-only like the rest of the listener; no auth by design.
 */

import type { IncomingMessage, ServerResponse } from "node:http";
import { basename } from "node:path";
import { getLastEditorStatus, type EnginePhase } from "../bridge/lifecycle.ts";

export interface StatusView {
  /** True when the last watchdog classification was not "down". */
  editorUp: boolean;
  phase: EnginePhase;
  pieActive: boolean;
  liveCodingInProgress: boolean;
  /** Basename of UNREAL_PROJECT_ROOT, or null when unset. */
  project: string | null;
  /** Epoch ms of the last watchdog probe, or null before the first one. */
  lastProbeAt: number | null;
  /** Pixel Streaming state; present only when the plugin reports it. */
  stream?: StreamView;
}

export interface StreamView {
  active: boolean;
  viewerPort: number | null;
  streamers: string[];
}

/** Map the bridge's snake_case `stream` field to the /status camelCase view.
 *  Returns undefined (field omitted) when the cached probe has no stream. */
function streamViewOf(status: Record<string, unknown> | null): StreamView | undefined {
  const stream = status?.stream;
  if (typeof stream !== "object" || stream === null || Array.isArray(stream)) return undefined;
  const s = stream as Record<string, unknown>;
  return {
    active: s.active === true,
    viewerPort: typeof s.viewer_port === "number" ? s.viewer_port : null,
    streamers: Array.isArray(s.streamers)
      ? s.streamers.filter((id): id is string => typeof id === "string")
      : [],
  };
}

function send(res: ServerResponse, code: number, body: unknown): void {
  const text = JSON.stringify(body);
  res.writeHead(code, { "Content-Type": "application/json" });
  res.end(text);
}

/** Handle a /status* request. Returns true if it owned the request. */
export async function handleStatusHttp(
  req: IncomingMessage,
  res: ServerResponse,
): Promise<boolean> {
  const pathname = (req.url ?? "").split("?")[0];
  if (!pathname || !pathname.startsWith("/status")) return false;

  if (req.method === "GET" && pathname === "/status") {
    const snap = getLastEditorStatus();
    const phase = snap?.phase ?? "down";
    // Same env-first convention as domains/editor.ts — config.ts does not
    // wrap UNREAL_PROJECT_ROOT.
    const projectRoot = process.env.UNREAL_PROJECT_ROOT;
    const view: StatusView = {
      editorUp: phase !== "down",
      phase,
      pieActive: snap?.status?.pie_active === true,
      liveCodingInProgress: snap?.status?.live_coding_in_progress === true,
      project: projectRoot ? basename(projectRoot.replace(/[\\/]+$/, "")) || null : null,
      lastProbeAt: snap?.probedAt ?? null,
    };
    const stream = streamViewOf(snap?.status ?? null);
    if (stream) view.stream = stream;
    send(res, 200, view);
    return true;
  }

  send(res, 404, { ok: false, error: `no status route for ${req.method} ${pathname}` });
  return true;
}
