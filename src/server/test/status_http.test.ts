/**
 * Unit tests for the plain-HTTP `GET /status` liveness surface
 * (`src/status/http.ts`). Arrange: seed the lifecycle watchdog's snapshot
 * cache through its `__test` hook — no live editor, no real TCP probe, no
 * waits. Act: real HTTP requests against a throwaway node:http server on an
 * ephemeral loopback port, wired exactly like main.ts. Assert one specific
 * status code + body per test.
 *
 * UNREAL_PROJECT_ROOT is saved/cleared/restored around every test (same
 * discipline as editor_build_game_target.test.ts) so the developer's real
 * environment neither leaks into assertions nor gets clobbered.
 */

import { expect, test, describe, beforeAll, afterAll, beforeEach, afterEach } from "bun:test";
import { createServer, type Server } from "node:http";
import type { AddressInfo } from "node:net";
import { handleStatusHttp } from "../src/status/http.ts";
import { __test as lifecycle } from "../src/bridge/lifecycle.ts";

let server: Server;
let base: string;

const savedProjectRoot = process.env.UNREAL_PROJECT_ROOT;

beforeAll(async () => {
  server = createServer((req, res) => {
    handleStatusHttp(req, res)
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

beforeEach(() => {
  lifecycle.setLastEditorStatus(null);
  delete process.env.UNREAL_PROJECT_ROOT;
});
afterEach(() => {
  lifecycle.setLastEditorStatus(null);
  if (savedProjectRoot === undefined) delete process.env.UNREAL_PROJECT_ROOT;
  else process.env.UNREAL_PROJECT_ROOT = savedProjectRoot;
});

describe("GET /status", () => {
  test("zero-state (watchdog has not probed yet): 200 with editorUp false, lastProbeAt null", async () => {
    const res = await fetch(`${base}/status`);
    expect(res.status).toBe(200);
    expect(res.headers.get("content-type")).toBe("application/json");
    const body: any = await res.json();
    expect(body).toEqual({
      editorUp: false,
      phase: "down",
      pieActive: false,
      liveCodingInProgress: false,
      project: null,
      lastProbeAt: null,
    });
  });

  test("interactive snapshot: editorUp true, PIE/live-coding flags surfaced verbatim", async () => {
    lifecycle.setLastEditorStatus({
      phase: "interactive",
      status: {
        ready: true,
        phase: "interactive",
        pie_active: true,
        live_coding_in_progress: false,
      },
      probedAt: 1_720_000_000_000,
    });
    const res = await fetch(`${base}/status`);
    expect(res.status).toBe(200);
    const body: any = await res.json();
    expect(body).toEqual({
      editorUp: true,
      phase: "interactive",
      pieActive: true,
      liveCodingInProgress: false,
      project: null,
      lastProbeAt: 1_720_000_000_000,
    });
  });

  test("initializing snapshot without a parsed result (partial-read probe): editorUp true, flags false", async () => {
    lifecycle.setLastEditorStatus({
      phase: "initializing",
      status: null,
      probedAt: 1_720_000_005_000,
    });
    const res = await fetch(`${base}/status`);
    expect(res.status).toBe(200);
    const body: any = await res.json();
    expect(body.editorUp).toBe(true);
    expect(body.phase).toBe("initializing");
    expect(body.pieActive).toBe(false);
    expect(body.liveCodingInProgress).toBe(false);
    expect(body.lastProbeAt).toBe(1_720_000_005_000);
  });

  test("editor went down after being up: editorUp flips back to false", async () => {
    lifecycle.setLastEditorStatus({
      phase: "down",
      status: null,
      probedAt: 1_720_000_010_000,
    });
    const res = await fetch(`${base}/status`);
    expect(res.status).toBe(200);
    const body: any = await res.json();
    expect(body.editorUp).toBe(false);
    expect(body.phase).toBe("down");
    expect(body.lastProbeAt).toBe(1_720_000_010_000); // stale-but-real probe time survives
  });

  test("project is the basename of UNREAL_PROJECT_ROOT when set", async () => {
    process.env.UNREAL_PROJECT_ROOT = "/home/dev/games/HoverBall";
    const res = await fetch(`${base}/status`);
    expect(res.status).toBe(200);
    const body: any = await res.json();
    expect(body.project).toBe("HoverBall");
  });
});

describe("/status non-routes", () => {
  test("unknown subpath: 404 with an ok:false error body", async () => {
    const res = await fetch(`${base}/status/extra`);
    expect(res.status).toBe(404);
    const body: any = await res.json();
    expect(body.ok).toBe(false);
    expect(body.error).toContain("no status route");
  });

  test("POST /status: 404 (read-only surface)", async () => {
    const res = await fetch(`${base}/status`, { method: "POST", body: "{}" });
    expect(res.status).toBe(404);
    const body: any = await res.json();
    expect(body.ok).toBe(false);
  });
});
