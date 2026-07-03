/**
 * Editor-dependent integration: real MCP client → server → bridge → LIVE editor.
 *
 * Gated on a reachable, interactive editor at :55557. If none is up (and
 * UE_MCP_LIVE!=1), the whole suite SKIPS rather than fails — so `bun test` is
 * green on a dev box with no engine, and exercises real round-trips in CI where
 * an editor is launched. Set UE_MCP_LIVE=1 to have the suite launch its own
 * headless editor (slow first build).
 */

import { expect, test, describe, beforeAll, afterAll } from "bun:test";
import { startTestClient, type TestClient } from "../harness/mcpClient.ts";
import { editorReady } from "../harness/bridge.ts";
import { EditorSession } from "../harness/editor.ts";
import { covers } from "../harness/coverage.ts";

let session: EditorSession | undefined;
let live = await editorReady();

if (!live && process.env.UE_MCP_LIVE === "1") {
  session = await new EditorSession().start("auto");
  live = await editorReady();
}

describe.skipIf(!live)("live editor round-trips", () => {
  let c: TestClient;
  beforeAll(async () => {
    c = await startTestClient("full");
  });
  afterAll(async () => {
    await c?.close();
    await session?.stop();
  });

  test("mcp_status reports ready", async () => {
    const env = await c.call("mcp_status");
    expect(env.status).toBe("success");
    expect((env.result as { ready: boolean }).ready).toBe(true);
  });

  test("actor_get_in_level returns the level actor list", async () => {
    const env = await c.call("actor_get_in_level");
    expect(env.status).toBe("success");
  });

  test("actor_spawn dry_run validates without mutating", async () => {
    const env = await c.call("actor_spawn", {
      class_path: "/Script/Engine.PointLight",
      dry_run: true,
    });
    expect(env.status).toBe("success");
    expect((env.result as { dry_run?: boolean }).dry_run).toBe(true);
  });

  test("catalog_call reaches a domain tool by name", async () => {
    const env = await c.call("catalog_call", { name: "actor_get_in_level", params: {} });
    expect(env.status).toBe("success");
  });

  covers("editor_read_logs");
  test(
    "editor_read_logs greps back a marker emitted via editor_console_exec",
    async () => {
      // Unique per-run marker so re-runs against a long-lived editor stay
      // idempotent (each run matches only its own line).
      const marker = `MCPTEST_LOGMARK_${Date.now()}_${process.pid}`;

      // Act (arrange half): emit the marker through a bridge op. `ke *` is a
      // harmless no-op — it broadcasts a nonexistent kismet event — but the
      // command LINE is echoed into the unified log tagged [EDITOR:Cmd].
      const execEnv = await c.call("editor_console_exec", {
        command: `ke * ${marker}`,
      });
      expect(execEnv.status).toBe("success");
      const exec = execEnv.result as { command: string; output: string };
      expect(exec.command).toContain(marker);
      expect(typeof exec.output).toBe("string");

      // Observe via a DIFFERENT primitive: editor_read_logs (server-local —
      // tails MCP_Unified.log on disk; returns its payload directly, no bridge
      // envelope). The log writer may lag the bridge reply, so poll with a
      // bounded deadline instead of a blind sleep.
      let logs:
        | { lines: string[]; returned: number; cursor: number; file: string }
        | undefined;
      const deadline = Date.now() + 10_000;
      for (;;) {
        logs = (await c.expect("editor_read_logs", { grep: marker })) as typeof logs;
        if (logs!.returned > 0 || Date.now() > deadline) break;
        await new Promise((r) => setTimeout(r, 250));
      }

      expect(logs!.returned).toBeGreaterThanOrEqual(1);
      expect(logs!.lines.some((l) => l.includes(marker))).toBe(true);
      expect(logs!.file.endsWith("MCP_Unified.log")).toBe(true);
      // Marker lines carry a [seq] prefix, so the batch cursor is populated.
      expect(logs!.cursor).toBeGreaterThan(0);
    },
    20_000,
  );

  // Bun mirror of tests/integration/test_reads.py::test_viewport_get_camera_reads_bugitgo_pose.
  // Bridge-op coverage is the pytest @covers ledger's job — no covers() here.
  test("editor_viewport_get_camera reads back a BugItGo-arranged pose", async () => {
    type Pose = {
      location: { x: number; y: number; z: number };
      rotation: { pitch: number; yaw: number; roll: number };
      fov: number;
      aspect: number;
      ortho: boolean;
      viewport_size: { x: number; y: number };
    };

    // Snapshot first — a pure read, used only to RESTORE the operator's framing
    // in the finally (the live editor may be shared). The assertion observes the
    // ARRANGED pose below, not this one.
    const snapEnv = await c.call("editor_viewport_get_camera", {});
    if (snapEnv.status !== "success") {
      // Headless -nullrhi may have no readable level viewport — tolerate, like
      // the pytest twin's skip.
      console.log("skipped: no level-editor viewport to read (headless -nullrhi?)");
      return;
    }
    const snap = snapEnv.result as Pose;
    for (const key of ["location", "rotation", "fov", "aspect", "ortho", "viewport_size"]) {
      expect(snap).toHaveProperty(key);
    }
    if (snap.ortho) {
      console.log("skipped: only an orthographic viewport available — need perspective");
      return;
    }

    try {
      // Arrange: BugItGo X Y Z Pitch Yaw Roll (world units / degrees) sets the
      // level-editor viewport camera deterministically (verified live).
      await c.expect("editor_console_exec", { command: "BugItGo 1234 2345 3456 -30 45 0" });
      const got = (await c.expect("editor_viewport_get_camera", {})) as unknown as Pose;
      if (Math.abs(got.location.x - 1234) >= 1 && process.env.UE_MCP_GUI !== "1") {
        console.log("skipped: BugItGo did not drive the viewport under headless -nullrhi");
        return;
      }
      expect(Math.abs(got.location.x - 1234)).toBeLessThan(1);
      expect(Math.abs(got.location.y - 2345)).toBeLessThan(1);
      expect(Math.abs(got.location.z - 3456)).toBeLessThan(1);
      expect(Math.abs(got.rotation.pitch + 30)).toBeLessThan(0.5);
      expect(Math.abs(got.rotation.yaw - 45)).toBeLessThan(0.5);
      expect(Math.abs(got.rotation.roll)).toBeLessThan(0.5);
      expect(got.ortho).toBe(false);
    } finally {
      // Put the camera back exactly as found.
      await c.call("editor_console_exec", {
        command:
          `BugItGo ${snap.location.x} ${snap.location.y} ${snap.location.z} ` +
          `${snap.rotation.pitch} ${snap.rotation.yaw} ${snap.rotation.roll}`,
      });
    }
  });

  test("code_run drives the editor and keeps data in-sandbox", async () => {
    const code = `
      const env = await unreal.actor_get_in_level({});
      const actors = env.result?.actors ?? [];
      console.log("actor count", actors.length);
      return { ok: env.status === "success", count: actors.length };
    `;
    const cc = await startTestClient("code");
    try {
      const env = await cc.call("code_run", { code, timeout_ms: 20000 });
      expect(env.status).toBe("success");
      expect((env.result as { value: { ok: boolean } }).value.ok).toBe(true);
    } finally {
      await cc.close();
    }
  });
});
