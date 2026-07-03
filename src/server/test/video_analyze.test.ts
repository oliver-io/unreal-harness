/**
 * Guard-path tests for `video_analyze` and the `pie_analyze` composite — the
 * headless, editor-free slice of the PIE video pipeline.
 *
 * What IS proven here (invoking the real ToolDef handlers from the canonical
 * registry, no mocks, no bridge — `ctx.conn` is deliberately undefined, so any
 * accidental bridge call throws instead of passing):
 *   - video_analyze refuses with `feature_disabled` when no Gemini key is
 *     configured, and that guard fires BEFORE any file I/O.
 *   - video_analyze refuses with `feature_disabled` on an unknown provider.
 *   - With a key configured, a missing input file is `invalid_path` and an
 *     over-budget analysis_fps is `invalid_argument` — both reachable without
 *     ever contacting the provider.
 *   - pie_analyze's earliest independently-observable guard: the PIE record
 *     lease guard (`pie_not_holder`) fires before the composite touches the
 *     bridge. Its OWN key check is the shared `analyzeVideoFile` guard proven
 *     above, but in the composite it sits after a live recording (bridge +
 *     PIE required), so it is not reachable headless.
 *
 * NOT proven (deferred, see docs/loops/tests/TASKS.md #DEFERRED): the positive
 * structured-verdict path — it requires the external video-understanding model.
 *
 * Env note: `config` snapshots process.env at module import (and backfills
 * from the repo-root .env), so manipulating env vars inside a test process
 * that has already imported the server is a no-op. The honest control point
 * is the config object itself — `as const` is type-level only — so each test
 * overrides the fields it needs and afterEach restores the originals for the
 * rest of the suite.
 */

import { expect, test, describe, beforeEach, afterEach } from "bun:test";
import { rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { buildRegistry } from "../src/register.ts";
import { config } from "../src/config.ts";
import type { ToolContext, ToolDef } from "../src/registry/types.ts";
import { acquire, __test as leaseTest } from "../src/pie/lease.ts";
import { covers } from "./harness/coverage.ts";

const ctx = { conn: undefined, sessionId: "video-guard-tests" } as unknown as ToolContext;

function tool(name: string): ToolDef {
  const def = buildRegistry().get(name);
  if (!def) throw new Error(`${name} not registered in the canonical registry`);
  return def;
}

/** Handler args with the schema defaults spelled out (we bypass MCP parsing). */
function videoArgs(over: Record<string, unknown>): Record<string, unknown> {
  return { path: "", expected_behavior: "x", criteria: [], analysis_fps: 0, model: "", ...over };
}

const mutableConfig = config as unknown as { geminiApiKey: string; videoProvider: string };
const saved = { key: config.geminiApiKey, provider: config.videoProvider };
const scratchFiles: string[] = [];

beforeEach(() => leaseTest.reset()); // lease state is module-global across test files
afterEach(() => {
  mutableConfig.geminiApiKey = saved.key;
  mutableConfig.videoProvider = saved.provider;
  leaseTest.reset();
  for (const f of scratchFiles.splice(0)) rmSync(f, { force: true });
});

describe("video_analyze guard chain (headless — no key, no provider contact)", () => {
  covers("video_analyze");

  test("no configured key → feature_disabled, before any file I/O", async () => {
    mutableConfig.geminiApiKey = "";
    const r: any = await tool("video_analyze").handler(
      // Nonexistent path on purpose: getting feature_disabled (not
      // invalid_path) proves the key guard is checked FIRST.
      videoArgs({ path: join(tmpdir(), "definitely-not-here.mp4") }),
      ctx,
    );
    expect(r.status).toBe("error");
    expect(r.error_code).toBe("feature_disabled");
    expect(r.error).toContain("GEMINI_API_KEY");
    expect(r.result).toBeUndefined();
  });

  test("unknown provider → feature_disabled naming the provider", async () => {
    mutableConfig.geminiApiKey = "test-key-never-used";
    mutableConfig.videoProvider = "acme";
    const r: any = await tool("video_analyze").handler(videoArgs({ path: "x.mp4" }), ctx);
    expect(r.status).toBe("error");
    expect(r.error_code).toBe("feature_disabled");
    expect(r.error).toContain("acme");
  });

  test("key configured but file missing → invalid_path naming the path", async () => {
    mutableConfig.geminiApiKey = "test-key-never-used";
    const path = join(tmpdir(), "mcp-video-guard-missing.mp4");
    const r: any = await tool("video_analyze").handler(videoArgs({ path }), ctx);
    expect(r.status).toBe("error");
    expect(r.error_code).toBe("invalid_path");
    expect(r.error).toContain(path);
  });

  test("analysis_fps over the cost guard → invalid_argument (file exists, key set)", async () => {
    mutableConfig.geminiApiKey = "test-key-never-used";
    const path = join(tmpdir(), `mcp-video-guard-${Date.now()}.mp4`);
    scratchFiles.push(path);
    await Bun.write(path, "not a real mp4 — never reaches the provider");
    const r: any = await tool("video_analyze").handler(
      videoArgs({ path, analysis_fps: config.videoMaxAnalysisFps + 1 }),
      ctx,
    );
    expect(r.status).toBe("error");
    expect(r.error_code).toBe("invalid_argument");
    expect(r.error).toContain(String(config.videoMaxAnalysisFps));
  });
});

describe("pie_analyze earliest guard (record lease — before any bridge call)", () => {
  covers("pie_analyze");

  test("someone else holds the PIE lease → pie_not_holder with the lease block", async () => {
    const fresh = new AbortController().signal;
    const got = await acquire("other-session", "otherAgent", fresh, 0);
    expect(got.outcome).toBe("acquired"); // arrange sanity

    // ctx.conn is undefined: if the guard did NOT fire first, the composite's
    // pie_record_start forward would throw rather than return this envelope.
    const r: any = await tool("pie_analyze").handler(
      {
        expected_behavior: "x",
        criteria: [],
        duration_s: 1,
        capture_fps: 30,
        width: 1280,
        height: 720,
        analysis_fps: 0,
        model: "",
      },
      ctx,
    );
    expect(r.status).toBe("error");
    expect(r.error_code).toBe("pie_not_holder");
    expect(r.error).toContain("pie_start");
    expect(r.result.pie_lease.state).toBe("not_holder");
    expect(r.result.pie_lease.you_hold).toBe(false);
    expect(r.result.pie_lease.holder.session_id).toBe("other-session");
    expect(r.result.pie_lease.holder.label).toBe("otherAgent");
  });
});
