/**
 * Env-driven configuration. Nothing project-specific lives here — every value is
 * either a generic local default or read from the environment. Credentials are
 * NEVER hardcoded; NEO4J_PASS has no default and must be supplied.
 */

export interface Config {
  mcpUrl: string;
  neo4jUri: string;
  neo4jUser: string;
  neo4jPass: string;
  dataDir: string;
}

/** Normalize an MCP base URL to the streamable-http endpoint (ends in /mcp). */
function mcpEndpoint(raw: string): string {
  const u = raw.replace(/\/+$/, "");
  return u.endsWith("/mcp") ? u : `${u}/mcp`;
}

export function loadConfig(overrides: Partial<Config> = {}): Config {
  const mcpUrl = mcpEndpoint(
    overrides.mcpUrl ?? process.env.MCP_URL ?? "http://127.0.0.1:8765",
  );
  const neo4jUri = overrides.neo4jUri ?? process.env.NEO4J_URI ?? "bolt://localhost:7687";
  const neo4jUser = overrides.neo4jUser ?? process.env.NEO4J_USER ?? "neo4j";
  const neo4jPass = overrides.neo4jPass ?? process.env.NEO4J_PASS ?? "";
  const dataDir = overrides.dataDir ?? process.env.UE_NEO4J_DATA_DIR ?? "tmp/data";
  return { mcpUrl, neo4jUri, neo4jUser, neo4jPass, dataDir };
}

/** Fail loudly if a graph operation is attempted without a password. */
export function requirePass(cfg: Config): void {
  if (!cfg.neo4jPass) {
    throw new Error(
      "NEO4J_PASS is not set. Export it before any graph operation, e.g.\n" +
        "  PowerShell:  $env:NEO4J_PASS = '<password>'\n" +
        "  bash:        export NEO4J_PASS='<password>'\n" +
        "(NEO4J_URI / NEO4J_USER are optional; defaults are bolt://localhost:7687 / neo4j.)",
    );
  }
}
