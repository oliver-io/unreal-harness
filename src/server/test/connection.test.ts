import { expect, test, describe, afterEach } from "bun:test";
import { UnrealConnection } from "../src/bridge/connection.ts";
import { bridgeOk, bridgeInner } from "../src/bridge/envelope.ts";

/**
 * A mock Unreal bridge. `reply(cmd)` returns the JSON the bridge would send for
 * a given command; `chunked` splits each reply into two writes to exercise the
 * read-until-parseable framing. Always answers `mcp_status` ready so the boot
 * gate opens.
 */
function startMockBridge(opts?: { chunked?: boolean }) {
  const server = Bun.listen({
    hostname: "127.0.0.1",
    port: 0,
    socket: {
      data(socket, data) {
        let cmd: { type: string; params: unknown };
        try {
          cmd = JSON.parse(new TextDecoder().decode(data));
        } catch {
          return;
        }
        const reply =
          cmd.type === "mcp_status"
            ? { status: "success", result: { ready: true } }
            : { status: "success", result: { echo: cmd.type, params: cmd.params } };
        const text = JSON.stringify(reply);
        if (opts?.chunked && text.length > 4) {
          const mid = Math.floor(text.length / 2);
          socket.write(text.slice(0, mid));
          setTimeout(() => socket.write(text.slice(mid)), 5);
        } else {
          socket.write(text);
        }
      },
    },
  });
  return { port: server.port, stop: () => server.stop(true) };
}

let mock: ReturnType<typeof startMockBridge> | undefined;
afterEach(() => {
  mock?.stop();
  mock = undefined;
});

describe("UnrealConnection", () => {
  test("round-trips a command through the boot gate", async () => {
    mock = startMockBridge();
    const conn = new UnrealConnection("127.0.0.1", mock.port);
    const env = await conn.sendCommand("actor_get_in_level", {});
    expect(bridgeOk(env)).toBe(true);
    expect(bridgeInner(env).echo).toBe("actor_get_in_level");
  });

  test("bypass commands skip the gate and reach the bridge directly", async () => {
    mock = startMockBridge();
    const conn = new UnrealConnection("127.0.0.1", mock.port);
    const env = await conn.sendCommand("mcp_status", {});
    expect(bridgeOk(env)).toBe(true);
    expect(bridgeInner(env).ready).toBe(true);
  });

  test("reassembles a reply delivered in multiple chunks", async () => {
    mock = startMockBridge({ chunked: true });
    const conn = new UnrealConnection("127.0.0.1", mock.port);
    const env = await conn.sendCommand("asset_list", { directory_path: "/Game/" });
    expect(bridgeOk(env)).toBe(true);
    expect(bridgeInner(env).echo).toBe("asset_list");
  });

  test("forwards params to the bridge", async () => {
    mock = startMockBridge();
    const conn = new UnrealConnection("127.0.0.1", mock.port);
    const env = await conn.sendCommand("actor_spawn", { class: "/Script/Engine.PointLight" });
    expect(bridgeInner(env).params).toEqual({ class: "/Script/Engine.PointLight" });
  });

  test("returns a clean error envelope when the editor is unreachable", async () => {
    // Nothing listening on this port.
    const conn = new UnrealConnection("127.0.0.1", 1);
    const env = await conn.sendCommand("actor_get_in_level", {});
    expect(env.status).toBe("error");
    expect(env.error).toMatch(/not running|not reachable|reachable/i);
  });

  test("does NOT retry a command once its payload was sent (GAP-003)", async () => {
    // A bridge that opens the boot gate, then accepts a real command's payload and
    // drops the connection WITHOUT replying. The payload reached the editor, so the
    // command must not be re-sent — re-sending a non-idempotent mutator duplicates it.
    let realCommandReceives = 0;
    const server = Bun.listen({
      hostname: "127.0.0.1",
      port: 0,
      socket: {
        data(socket, data) {
          let cmd: { type: string };
          try {
            cmd = JSON.parse(new TextDecoder().decode(data));
          } catch {
            return;
          }
          if (cmd.type === "mcp_status") {
            socket.write(JSON.stringify({ status: "success", result: { ready: true } }));
            return;
          }
          // Real command: count the delivery, then drop the socket with no reply.
          realCommandReceives += 1;
          socket.end();
        },
      },
    });
    try {
      const conn = new UnrealConnection("127.0.0.1", server.port);
      const env = await conn.sendCommand("asset_import_mesh", { path: "/Game/x.fbx" });
      expect(env.status).toBe("error");
      // Sent exactly once — the post-send failure was not retried.
      expect(realCommandReceives).toBe(1);
      expect(env.error).toMatch(/not retried|still be executing/i);
    } finally {
      server.stop(true);
    }
  });
});
