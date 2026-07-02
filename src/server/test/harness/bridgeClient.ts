/**
 * Raw bridge client for integration tests — TS port of
 * `tests/harness/bridge_client.py`. Speaks JSON directly to the C++ plugin on
 * :55557, skipping the MCP tool layer (no Zod validation / arg mapping). Tests
 * use this for `bridge.*` calls (wire-command names, e.g. `st_add_state`) and
 * the in-process MCP client for `mcp.*` calls (tool names, e.g. `statetree_*`).
 *
 * Built on the server's own UnrealConnection so framing / retry / boot gate are
 * identical to production.
 */

import { UnrealConnection } from "../../src/bridge/connection.ts";
import { CommandError } from "./mcpClient.ts";
import type { Envelope } from "../../src/bridge/envelope.ts";
import { BRIDGE_HOST, BRIDGE_PORT } from "./env.ts";

export class RawBridge {
  private conn = new UnrealConnection(BRIDGE_HOST, BRIDGE_PORT);

  /** Send one command; return the full envelope. Never throws on status:"error". */
  command(type: string, params: Record<string, unknown> = {}): Promise<Envelope> {
    return this.conn.sendCommand(type, params);
  }

  /** Like command but require success; returns the inner `result` object. */
  async expect(type: string, params: Record<string, unknown> = {}): Promise<Record<string, unknown>> {
    const resp = await this.command(type, params);
    if (resp.status !== "success") throw new CommandError(type, resp);
    const result = resp.result;
    return result && typeof result === "object" && !Array.isArray(result)
      ? (result as Record<string, unknown>)
      : {};
  }

  async isReady(): Promise<boolean> {
    try {
      const r = await this.command("mcp_status", {});
      return (r.result as { ready?: boolean })?.ready === true;
    } catch {
      return false;
    }
  }

  ping(): Promise<Envelope> {
    return this.command("ping", {});
  }
}
