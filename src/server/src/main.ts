/**
 * Entry point. Builds the registry + MCP server and serves streamable-HTTP on
 * `:8765/mcp`. Runs natively on Bun (via node:http, which Bun implements).
 *
 *   bun run mcp          # start
 *   UNREAL_MCP_PORT=9000 bun run mcp
 */

import { createServer, type IncomingMessage, type ServerResponse } from "node:http";
import { randomUUID } from "node:crypto";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";
import { isInitializeRequest } from "@modelcontextprotocol/sdk/types.js";
import { config } from "./config.ts";
import { buildRegistry } from "./register.ts";
import { buildServer } from "./server.ts";
import { startLifecycleGuards } from "./bridge/lifecycle.ts";
import { onSessionClosed } from "./pie/lease.ts";
import { startPieReconciler } from "./pie/reconciler.ts";
import { handleBuildHttp } from "./build/http.ts";
import { log } from "./log.ts";

const MCP_PATH = "/mcp";

const registry = buildRegistry();

// One session → one transport. Local single-user MCP; sessions keyed by the
// SDK-issued id. Each new session gets its own server instance.
const transports = new Map<string, StreamableHTTPServerTransport>();

async function handleMcp(req: IncomingMessage, res: ServerResponse): Promise<void> {
  const sessionId = req.headers["mcp-session-id"];
  const id = Array.isArray(sessionId) ? sessionId[0] : sessionId;

  if (req.method === "POST") {
    const body = await readJson(req);
    let transport = id ? transports.get(id) : undefined;

    if (!transport && isInitializeRequest(body)) {
      transport = new StreamableHTTPServerTransport({
        sessionIdGenerator: () => randomUUID(),
        onsessioninitialized: (sid) => {
          transports.set(sid, transport!);
        },
      });
      transport.onclose = () => {
        const sid = transport!.sessionId;
        if (sid) {
          transports.delete(sid);
          // A disconnected agent must not wedge the PIE lease: release its lease
          // / drop its queue slot. A reconnect is a new session → back of line.
          onSessionClosed(sid);
        }
      };
      await buildServer(registry).connect(transport);
    }

    if (!transport) {
      // A session id was supplied but we don't know it (server restarted, session
      // expired). Per the MCP streamable-HTTP spec a client re-initializes only on
      // 404 for a stale Mcp-Session-Id — a 400 just surfaces the error and the
      // client stays wedged on the dead session. Return 404 so it auto-recovers.
      // No id at all and not an initialize → genuine bad request (400).
      res.writeHead(id ? 404 : 400, { "Content-Type": "application/json" });
      res.end(JSON.stringify(rpcError("No valid session; send an initialize request first.")));
      return;
    }
    await transport.handleRequest(req, res, body);
    return;
  }

  // GET (SSE stream) / DELETE (session teardown) route to the live transport.
  if ((req.method === "GET" || req.method === "DELETE") && id && transports.has(id)) {
    await transports.get(id)!.handleRequest(req, res);
    return;
  }

  res.writeHead(400, { "Content-Type": "application/json" });
  res.end(JSON.stringify(rpcError("Bad request: unknown session or method.")));
}

const httpServer = createServer((req, res) => {
  const url = req.url ?? "";
  // Plain-HTTP build-lock surface for the build scripts (not MCP JSON-RPC).
  if (url.startsWith("/build")) {
    handleBuildHttp(req, res).catch((err) => {
      log.error(`build request failed: ${err instanceof Error ? err.message : String(err)}`);
      if (!res.headersSent) res.writeHead(500, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ ok: false, error: "internal server error" }));
    });
    return;
  }
  if (!url.startsWith(MCP_PATH)) {
    res.writeHead(404).end();
    return;
  }
  handleMcp(req, res).catch((err) => {
    log.error(`request failed: ${err instanceof Error ? err.message : String(err)}`);
    if (!res.headersSent) res.writeHead(500, { "Content-Type": "application/json" });
    res.end(JSON.stringify(rpcError("Internal server error.")));
  });
});

const stopPieReconciler = startPieReconciler();

startLifecycleGuards(() => {
  stopPieReconciler();
  for (const t of transports.values()) void t.close();
});

httpServer.listen(config.mcpPort, config.mcpHost, () => {
  const exposed = registry.all().filter((d) =>
    config.surface === "compact"
      ? ["core", "catalog", "result"].includes(d.domain)
      : config.surface === "code"
        ? ["core", "catalog", "result", "code"].includes(d.domain)
        : d.domain !== "code",
  ).length;
  log.info(
    `Unreal MCP listening on http://${config.mcpHost}:${config.mcpPort}${MCP_PATH} ` +
      `→ bridge ${config.bridgeHost}:${config.bridgePort} ` +
      `[surface=${config.surface}, ${exposed}/${registry.size()} tools advertised]`,
  );
});

// ── helpers ──────────────────────────────────────────────────────────

function readJson(req: IncomingMessage): Promise<unknown> {
  return new Promise((resolve, reject) => {
    const chunks: Buffer[] = [];
    req.on("data", (c: Buffer) => chunks.push(c));
    req.on("end", () => {
      const raw = Buffer.concat(chunks).toString("utf-8");
      if (!raw) return resolve(undefined);
      try {
        resolve(JSON.parse(raw));
      } catch (e) {
        reject(e);
      }
    });
    req.on("error", reject);
  });
}

function rpcError(message: string) {
  return { jsonrpc: "2.0", error: { code: -32000, message }, id: null };
}
