/**
 * Plain-HTTP surface for the build lock, for the build SCRIPTS to call.
 *
 * Build scripts are shell processes; making them speak full MCP JSON-RPC (init
 * handshake + session id + tools/call) would be miserable. Instead they hit these
 * tiny JSON endpoints with one `Invoke-RestMethod` / `curl`:
 *
 *   POST /build/acquire   { pid, target?, label?, host?, ttl_seconds? }
 *       → { ok, status:"acquired"|"busy", build|holder, message }   (always 200)
 *   POST /build/release   { build_id?, pid? }      → { ok, status }
 *   POST /build/heartbeat { build_id?|pid, ttl_seconds? } → { ok, build }
 *   GET  /build/status                              → { in_progress, holder }
 *
 * `acquire` returns 200 with `status:"busy"` (not an HTTP error) so a script can
 * branch on a field instead of catching a non-2xx throw. 400 only for malformed input.
 */

import type { IncomingMessage, ServerResponse } from "node:http";
import { acquire, release, heartbeat, status } from "./lock.ts";

function send(res: ServerResponse, code: number, body: unknown): void {
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

const numOf = (v: unknown): number | undefined => {
  const n = typeof v === "number" ? v : typeof v === "string" ? Number(v) : NaN;
  return Number.isFinite(n) ? n : undefined;
};
const strOf = (v: unknown): string | undefined => (typeof v === "string" ? v : undefined);

/** Handle a /build/* request. Returns true if it owned the request. */
export async function handleBuildHttp(
  req: IncomingMessage,
  res: ServerResponse,
): Promise<boolean> {
  const pathname = (req.url ?? "").split("?")[0];
  if (!pathname || !pathname.startsWith("/build")) return false;

  if (req.method === "GET" && pathname === "/build/status") {
    send(res, 200, status());
    return true;
  }

  if (req.method === "POST") {
    const body = await readJson(req);

    if (pathname === "/build/acquire") {
      const pid = numOf(body.pid);
      if (pid === undefined) {
        send(res, 400, { ok: false, error: "missing or invalid 'pid'" });
        return true;
      }
      const ttlSec = numOf(body.ttl_seconds);
      const r = acquire({
        pid,
        target: strOf(body.target),
        label: strOf(body.label),
        host: strOf(body.host),
        ttlMs: ttlSec !== undefined ? ttlSec * 1000 : undefined,
      });
      const ok = r.outcome === "acquired";
      send(res, 200, {
        ok,
        status: r.outcome,
        fresh: r.fresh,
        build: ok ? r.build : undefined,
        holder: r.build,
        message: ok
          ? `Build lock acquired (build_id ${r.build.build_id}).`
          : `Another build is in progress: ${r.build.label} (pid ${r.build.pid}` +
            `${r.build.target ? `, target ${r.build.target}` : ""}), started ` +
            `${Math.round(r.build.held_ms / 1000)}s ago.`,
      });
      return true;
    }

    if (pathname === "/build/release") {
      const r = release({ buildId: strOf(body.build_id), pid: numOf(body.pid) });
      send(res, 200, { ok: r.outcome !== "not_holder", status: r.outcome });
      return true;
    }

    if (pathname === "/build/heartbeat") {
      const ttlSec = numOf(body.ttl_seconds);
      const r = heartbeat({
        buildId: strOf(body.build_id),
        pid: numOf(body.pid),
        ttlMs: ttlSec !== undefined ? ttlSec * 1000 : undefined,
      });
      send(res, 200, r);
      return true;
    }
  }

  send(res, 404, { ok: false, error: `no build route for ${req.method} ${pathname}` });
  return true;
}
