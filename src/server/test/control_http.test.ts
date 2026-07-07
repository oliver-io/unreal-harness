/**
 * Unit tests for the plain-HTTP Pixel Streaming control surface
 * (`src/control/http.ts` — portable.dev#19 M2). Arrange: swap the module's
 * bridge-dispatch seam (`__test.setSend`) for a recording stub — no live
 * editor, no real TCP. Act: real HTTP requests against a throwaway node:http
 * server on an ephemeral loopback port, wired exactly like main.ts. Assert one
 * specific status code + body per test (repo test-quality rule: never accept
 * multiple outcomes).
 *
 * The stream_* wire commands are bridge operations (C++ dispatch keys), so
 * their end-to-end coverage is the pytest parity oracle's job — these tests
 * own the HTTP shim: routing, wire params, and envelope→HTTP mapping.
 */

import { expect, test, describe, beforeAll, afterAll, afterEach } from "bun:test";
import { createServer, type Server } from "node:http";
import type { AddressInfo } from "node:net";
import { handleControlHttp, __test as control } from "../src/control/http.ts";
import type { Envelope } from "../src/bridge/envelope.ts";

let server: Server;
let base: string;

/** Install a send stub that records calls and replies with `env`. */
function stubSend(env: Envelope) {
  const calls: Array<{ command: string; params: Record<string, unknown> | undefined }> = [];
  control.setSend(async (command, params) => {
    calls.push({ command, params });
    return env;
  });
  return calls;
}

beforeAll(async () => {
  server = createServer((req, res) => {
    handleControlHttp(req, res)
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

afterEach(() => {
  control.resetSend();
});

describe("POST /control/stream/start", () => {
  test("happy path: sends wire stream_start with defaults, 200 with the exact contract body", async () => {
    const calls = stubSend({ status: "success", result: { state: "starting" } });
    const res = await fetch(`${base}/control/stream/start`, {
      method: "POST",
      body: JSON.stringify({ viewerPort: 8890 }),
    });
    expect(res.status).toBe(200);
    expect(res.headers.get("content-type")).toBe("application/json");
    const body: any = await res.json();
    expect(body).toEqual({ ok: true, viewerPort: 8890, streamerPort: 8888, state: "starting" });
    expect(calls).toEqual([
      { command: "stream_start", params: { viewer_port: 8890, streamer_port: 8888 } },
    ]);
  });

  test("empty body: viewerPort defaults to 8890 on the wire and in the response", async () => {
    const calls = stubSend({ status: "success", result: {} });
    const res = await fetch(`${base}/control/stream/start`, { method: "POST" });
    expect(res.status).toBe(200);
    const body: any = await res.json();
    expect(body).toEqual({ ok: true, viewerPort: 8890, streamerPort: 8888, state: "starting" });
    expect(calls[0]!.params).toEqual({ viewer_port: 8890, streamer_port: 8888 });
  });

  test("custom viewerPort is forwarded as wire viewer_port and echoed back", async () => {
    const calls = stubSend({ status: "success", result: {} });
    const res = await fetch(`${base}/control/stream/start`, {
      method: "POST",
      body: JSON.stringify({ viewerPort: 9001 }),
    });
    expect(res.status).toBe(200);
    const body: any = await res.json();
    expect(body.viewerPort).toBe(9001);
    expect(calls[0]!.params).toEqual({ viewer_port: 9001, streamer_port: 8888 });
  });

  test("editor down (bridge error envelope): 503 with ok:false and the bridge's error text", async () => {
    stubSend({
      status: "error",
      error: "Failed to connect to Unreal Engine — editor not running or not reachable on 127.0.0.1:55557.",
    });
    const res = await fetch(`${base}/control/stream/start`, {
      method: "POST",
      body: JSON.stringify({ viewerPort: 8890 }),
    });
    expect(res.status).toBe(503);
    const body: any = await res.json();
    expect(body.ok).toBe(false);
    expect(body.error).toContain("Failed to connect to Unreal Engine");
  });

  test("C++ error envelope (feature_disabled): also 503 with the handler's message", async () => {
    stubSend({
      status: "error",
      error: "Pixel Streaming 2 is unavailable — the PixelStreaming2 editor module is not loaded.",
      error_code: "feature_disabled",
    });
    const res = await fetch(`${base}/control/stream/start`, { method: "POST", body: "{}" });
    expect(res.status).toBe(503);
    const body: any = await res.json();
    expect(body.ok).toBe(false);
    expect(body.error).toContain("Pixel Streaming 2 is unavailable");
  });

  test("non-integer viewerPort: 400, nothing sent to the bridge", async () => {
    const calls = stubSend({ status: "success", result: {} });
    const res = await fetch(`${base}/control/stream/start`, {
      method: "POST",
      body: JSON.stringify({ viewerPort: "8890" }),
    });
    expect(res.status).toBe(400);
    const body: any = await res.json();
    expect(body.ok).toBe(false);
    expect(body.error).toContain("viewerPort");
    expect(calls).toHaveLength(0);
  });
});

describe("POST /control/stream/stop", () => {
  test("sends wire stream_stop; 200 {ok:true} — including the idempotent nothing-was-streaming case", async () => {
    const calls = stubSend({
      status: "success",
      result: { stopped: true, was_streaming: false },
    });
    const res = await fetch(`${base}/control/stream/stop`, { method: "POST" });
    expect(res.status).toBe(200);
    const body: any = await res.json();
    expect(body).toEqual({ ok: true });
    expect(calls).toEqual([{ command: "stream_stop", params: {} }]);
  });

  test("editor down: 503 with ok:false and the bridge's error text", async () => {
    stubSend({
      status: "error",
      error: "Failed to connect to Unreal Engine — editor not running or not reachable on 127.0.0.1:55557.",
    });
    const res = await fetch(`${base}/control/stream/stop`, { method: "POST" });
    expect(res.status).toBe(503);
    const body: any = await res.json();
    expect(body.ok).toBe(false);
    expect(body.error).toContain("Failed to connect to Unreal Engine");
  });
});

describe("/control non-routes", () => {
  test("unknown control path: 404 with an ok:false error body", async () => {
    const calls = stubSend({ status: "success", result: {} });
    const res = await fetch(`${base}/control/bogus`, { method: "POST", body: "{}" });
    expect(res.status).toBe(404);
    const body: any = await res.json();
    expect(body.ok).toBe(false);
    expect(body.error).toContain("no control route");
    expect(calls).toHaveLength(0);
  });

  test("GET /control/stream/start (wrong method): 404, nothing sent to the bridge", async () => {
    const calls = stubSend({ status: "success", result: {} });
    const res = await fetch(`${base}/control/stream/start`);
    expect(res.status).toBe(404);
    const body: any = await res.json();
    expect(body.ok).toBe(false);
    expect(calls).toHaveLength(0);
  });
});
