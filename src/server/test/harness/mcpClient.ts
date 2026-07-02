/**
 * In-process MCP client for integration tests.
 *
 * Instead of spawning `bun run mcp` over HTTP (a separate process that can leak —
 * the bug class that plagued the Python harness), we link a real MCP Client and
 * the real server over the SDK's in-memory transport. This still exercises the
 * FULL product path — MCP protocol → registry exposure/validation → handler →
 * bridge — just without a socket or a child process. Teardown is a function
 * call; nothing to tree-kill.
 *
 * Tools that talk to the editor still need a live editor on :55557; tools that
 * don't (catalog_*, code_api) work with no editor at all.
 */

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { buildRegistry } from "../../src/register.ts";
import { buildServer } from "../../src/server.ts";
import type { Envelope } from "../../src/bridge/envelope.ts";

export class CommandError extends Error {
  constructor(
    public tool: string,
    public payload: unknown,
  ) {
    super(`${tool}: ${JSON.stringify(payload)?.slice(0, 300)}`);
    this.name = "CommandError";
  }
}

export interface TestClient {
  /** Call a tool by name; returns the parsed bridge envelope (never throws on a
   *  status:"error" envelope). Alias: `command`. */
  call(name: string, args?: Record<string, unknown>): Promise<Envelope>;
  command(name: string, args?: Record<string, unknown>): Promise<Envelope>;
  /** Call a tool and require success; returns the inner `result` object.
   *  Throws {@link CommandError} on a status:"error" / success:false envelope.
   *  Mirrors the pytest `mcp.expect(...)`. */
  expect(name: string, args?: Record<string, unknown>): Promise<Record<string, unknown>>;
  /** True iff the editor reports interactive via mcp_status. */
  isReady(): Promise<boolean>;
  /** Names of all advertised tools (post surface-mode filtering). */
  listToolNames(): Promise<string[]>;
  close(): Promise<void>;
}

/** Unwrap an envelope the way pytest's mcp.expect does: throw on error, else
 *  return the inner result object (or the envelope if there's no inner dict). */
export function expectResult(tool: string, env: Envelope): Record<string, unknown> {
  const success = (env as { success?: unknown }).success;
  if (env.status === "error" || success === false) throw new CommandError(tool, env);
  const inner = env.result;
  if (env.status === "success" && inner && typeof inner === "object" && !Array.isArray(inner)) {
    return inner as Record<string, unknown>;
  }
  return env as unknown as Record<string, unknown>;
}

export async function startTestClient(
  surface: "full" | "compact" | "code" = "full",
): Promise<TestClient> {
  const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
  const server = buildServer(buildRegistry(), { surface });
  await server.connect(serverTransport);

  const client = new Client({ name: "unreal-mcp-test", version: "0.0.0" });
  await client.connect(clientTransport);

  const call = async (name: string, args: Record<string, unknown> = {}): Promise<Envelope> => {
    const res = await client.callTool({ name, arguments: args });
    const content = (res.content ?? []) as { type: string; text?: string }[];
    const text = content.find((c) => c.type === "text")?.text ?? "{}";
    return JSON.parse(text) as Envelope;
  };

  return {
    call,
    command: call,
    async expect(name, args = {}) {
      return expectResult(name, await call(name, args));
    },
    async isReady() {
      try {
        const env = await call("mcp_status", {});
        return (env.result as { ready?: boolean })?.ready === true;
      } catch {
        return false;
      }
    },
    async listToolNames() {
      const { tools } = await client.listTools();
      return tools.map((t) => t.name).sort();
    },
    async close() {
      // Tolerant: the linked in-memory pair can race a close (one side reports
      // "Connection closed" as the other tears down). Teardown must not fail a suite.
      try {
        await client.close();
      } catch {
        /* ignore */
      }
      try {
        await server.close();
      } catch {
        /* ignore */
      }
    },
  };
}
