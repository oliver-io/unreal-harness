/**
 * Unit tests for the plain-HTTP self-describing StreamSourceDescriptor surface
 * (`src/descriptor/http.ts`). Act: real HTTP requests against a throwaway
 * node:http server on an ephemeral loopback port, wired exactly like main.ts.
 * Assert one specific outcome per test (repo test-quality rule).
 */

import { expect, test, describe, beforeAll, afterAll } from "bun:test";
import { createServer, type Server } from "node:http";
import type { AddressInfo } from "node:net";
import { handleDescriptorHttp } from "../src/descriptor/http.ts";
import { config } from "../src/config.ts";

let server: Server;
let base: string;

beforeAll(async () => {
  server = createServer((req, res) => {
    handleDescriptorHttp(req, res)
      .then((owned) => {
        if (!owned) res.writeHead(404).end();
      })
      .catch(() => {
        if (!res.headersSent) res.writeHead(500);
        res.end();
      });
  });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  base = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
});
afterAll(async () => {
  await new Promise((resolve) => server.close(resolve));
});

describe("GET /stream-source", () => {
  test("serves the neutral descriptor with all required keys", async () => {
    const res = await fetch(`${base}/stream-source`);
    expect(res.status).toBe(200);
    expect(res.headers.get("content-type")).toBe("application/json");
    const body: any = await res.json();

    expect(body.version).toBe(1);
    expect(typeof body.sourceId).toBe("string");
    expect(body.sourceId.length).toBeGreaterThan(0);
    expect(body.sourceKind).toBe("pixelstreaming2");
    expect(body.control.baseUrl).toBe(`http://127.0.0.1:${config.mcpPort}`);
    expect(body.control.status).toBe("/status");
    expect(body.control.start).toBe("/control/stream/start");
    expect(body.control.stop).toBe("/control/stream/stop");
    expect(body.control.descriptor).toBe("/stream-source");
    expect(body.player.type).toBe("pixelstreaming2");
    expect(body.player.viewerPort).toBe(config.viewerPort);
    expect(body.input.scheme).toBe("ps2-uiinteraction");
    expect(body.input.controls).toEqual([
      "look",
      "pan",
      "dolly",
      "orbit",
      "tap",
      "focus",
      "pie",
    ]);
  });

  test("sourceId defaults to source:<mcpPort> when unconfigured", async () => {
    // The repo default leaves UNREAL_MCP_STREAM_SOURCE_ID unset.
    if (config.streamSourceId) return; // configured env — skip the default assertion
    const res = await fetch(`${base}/stream-source`);
    const body: any = await res.json();
    expect(body.sourceId).toBe(`source:${config.mcpPort}`);
  });

  test("POST /stream-source: 404 (read-only surface)", async () => {
    const res = await fetch(`${base}/stream-source`, { method: "POST", body: "{}" });
    expect(res.status).toBe(404);
    const body: any = await res.json();
    expect(body.ok).toBe(false);
    expect(body.error).toContain("no stream-source route");
  });
});
