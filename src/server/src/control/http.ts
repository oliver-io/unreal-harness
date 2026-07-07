/**
 * Plain-HTTP control surface for Pixel Streaming, for an EXTERNAL local
 * process (the Portable backend — portable.dev#19 M2) to drive without an MCP
 * session. Same dispatch style as `build/http.ts` / `status/http.ts`; routed
 * from `main.ts`. Loopback-only like the rest of the listener; no auth by
 * design.
 *
 *   POST /control/stream/start  { viewerPort?: number }
 *     → 200 { ok:true, viewerPort, streamerPort: 8888, state: "starting" }
 *     → 503 { ok:false, error } when the editor is down / the bridge refused /
 *       the C++ handler returned an error envelope. (Portable treats any
 *       non-200 as failure, so every failure mode maps to 503 + error text.)
 *   POST /control/stream/stop
 *     → 200 { ok:true } (idempotent: stopping an idle editor still succeeds)
 *     → 503 { ok:false, error } when the editor is unreachable
 *   anything else under /control → 404 { ok:false, error }
 *
 * Unlike /status this is NOT cache-backed: each call is a real bridge command
 * (`stream_start` / `stream_stop`), so start can also block briefly at the
 * server-side boot gate while the editor initializes.
 */

import type { IncomingMessage, ServerResponse } from "node:http";
import { getConnection } from "../bridge/connection.ts";
import type { Envelope } from "../bridge/envelope.ts";

const DEFAULT_VIEWER_PORT = 8890;
const DEFAULT_STREAMER_PORT = 8888;

/** Bridge dispatch seam — swappable in tests (no live editor / TCP). */
type SendFn = (command: string, params?: Record<string, unknown>) => Promise<Envelope>;
const realSend: SendFn = (command, params) => getConnection().sendCommand(command, params);
let send: SendFn = realSend;

function sendJson(res: ServerResponse, code: number, body: unknown): void {
  const text = JSON.stringify(body);
  res.writeHead(code, { "Content-Type": "application/json" });
  res.end(text);
}

function readJson(req: IncomingMessage): Promise<Record<string, unknown>> {
  return new Promise((resolve, reject) => {
    const chunks: Buffer[] = [];
    req.on("data", (c: Buffer) => chunks.push(c));
    req.on("end", () => {
      const raw = Buffer.concat(chunks).toString("utf-8");
      if (!raw) return resolve({});
      try {
        const parsed = JSON.parse(raw);
        resolve(parsed && typeof parsed === "object" ? parsed : {});
      } catch (e) {
        reject(e);
      }
    });
    req.on("error", reject);
  });
}

/** Handle a /control/* request. Returns true if it owned the request. */
export async function handleControlHttp(
  req: IncomingMessage,
  res: ServerResponse,
): Promise<boolean> {
  const pathname = (req.url ?? "").split("?")[0];
  if (!pathname || !pathname.startsWith("/control")) return false;

  if (req.method === "POST" && pathname === "/control/stream/start") {
    let body: Record<string, unknown>;
    try {
      body = await readJson(req);
    } catch {
      sendJson(res, 400, { ok: false, error: "malformed JSON body" });
      return true;
    }
    let viewerPort = DEFAULT_VIEWER_PORT;
    if (body.viewerPort !== undefined) {
      const v = body.viewerPort;
      if (typeof v !== "number" || !Number.isInteger(v) || v < 1 || v > 65535) {
        sendJson(res, 400, { ok: false, error: "'viewerPort' must be an integer in 1-65535" });
        return true;
      }
      viewerPort = v;
    }

    const env = await send("stream_start", {
      viewer_port: viewerPort,
      streamer_port: DEFAULT_STREAMER_PORT,
    });
    if (env.status !== "success") {
      // Editor down, bridge refused, or a C++ error envelope (e.g.
      // feature_disabled) — Portable branches on non-200, so all map to 503.
      sendJson(res, 503, { ok: false, error: env.error ?? "stream_start failed" });
      return true;
    }
    sendJson(res, 200, {
      ok: true,
      viewerPort,
      streamerPort: DEFAULT_STREAMER_PORT,
      state: "starting",
    });
    return true;
  }

  if (req.method === "POST" && pathname === "/control/stream/stop") {
    const env = await send("stream_stop", {});
    if (env.status !== "success") {
      sendJson(res, 503, { ok: false, error: env.error ?? "stream_stop failed" });
      return true;
    }
    sendJson(res, 200, { ok: true });
    return true;
  }

  sendJson(res, 404, { ok: false, error: `no control route for ${req.method} ${pathname}` });
  return true;
}

// ── test hooks ──────────────────────────────────────────────────────────
export const __test = {
  setSend(fn: SendFn) {
    send = fn;
  },
  resetSend() {
    send = realSend;
  },
};
