/**
 * Thin MCP client over the harness's streamable-http server. Calls a tool and
 * unwraps the uniform response envelope ({status, result, error, error_code}),
 * returning `result` on success and throwing a readable error otherwise.
 */

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

export class Mcp {
  private client: Client | null = null;

  constructor(private readonly url: string) {}

  async connect(): Promise<void> {
    const transport = new StreamableHTTPClientTransport(new URL(this.url));
    this.client = new Client(
      { name: "ue-neo4j-ingest", version: "1.0.0" },
      { capabilities: {} },
    );
    try {
      await this.client.connect(transport);
    } catch (e) {
      throw new Error(
        `Cannot reach the MCP server at ${this.url}. Is the harness server running ` +
          `(scripts/run-server) and the editor booted? Underlying: ${(e as Error).message}`,
      );
    }
  }

  /** Call a tool; returns the envelope's `result` (or the whole envelope if it has none). */
  async call(name: string, args: Record<string, unknown>): Promise<any> {
    if (!this.client) throw new Error("MCP client not connected — call connect() first.");
    const res: any = await this.client.callTool({ name, arguments: args });
    const block = Array.isArray(res?.content) ? res.content.find((c: any) => c?.type === "text") : null;
    const text: string = block?.text ?? "";
    let env: any;
    try {
      env = JSON.parse(text);
    } catch {
      throw new Error(`Tool ${name} returned non-JSON content: ${text.slice(0, 200)}`);
    }
    if (env && env.status === "error") {
      const code = env.error_code ? `[${env.error_code}] ` : "";
      throw new Error(`Tool ${name} failed: ${code}${env.error ?? "unknown error"}`);
    }
    // Peel the bridge envelope ({status, result}) when present…
    let r = env && typeof env === "object" && "result" in env ? env.result : env;
    // …then the common {success, data} handler convention some commands use.
    if (r && typeof r === "object" && typeof r.success === "boolean" && "data" in r) {
      if (!r.success) {
        throw new Error(`Tool ${name} failed: ${r.error ?? r.message ?? "success=false"}`);
      }
      r = r.data;
    }
    return r;
  }

  async close(): Promise<void> {
    await this.client?.close();
    this.client = null;
  }
}
