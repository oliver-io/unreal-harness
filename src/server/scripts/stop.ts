/**
 * `bun run mcp:stop` — free the MCP port by tree-killing whatever listens on it.
 * Dependency-free (netstat on Windows, lsof elsewhere). Idempotent.
 */

import { config } from "../src/config.ts";

const port = config.mcpPort;

async function pidsOnPort(p: number): Promise<number[]> {
  const isWin = process.platform === "win32";
  const cmd = isWin
    ? ["netstat", "-ano", "-p", "TCP"]
    : ["lsof", "-tiTCP:" + p, "-sTCP:LISTEN"];
  const out = await new Response(Bun.spawn(cmd, { stderr: "ignore" }).stdout).text();
  const pids = new Set<number>();
  if (isWin) {
    for (const line of out.split("\n")) {
      const parts = line.trim().split(/\s+/);
      if (parts[0] === "TCP" && parts.includes("LISTENING") && parts[1]?.endsWith(":" + p)) {
        const pid = Number(parts.at(-1));
        if (Number.isFinite(pid)) pids.add(pid);
      }
    }
  } else {
    for (const tok of out.split(/\s+/)) {
      const pid = Number(tok);
      if (Number.isFinite(pid) && pid > 0) pids.add(pid);
    }
  }
  return [...pids];
}

function killTree(pid: number): void {
  const cmd =
    process.platform === "win32"
      ? ["taskkill", "/F", "/T", "/PID", String(pid)]
      : ["kill", "-9", String(pid)];
  Bun.spawnSync(cmd, { stderr: "ignore", stdout: "ignore" });
}

const pids = await pidsOnPort(port);
if (pids.length === 0) {
  console.log(`nothing listening on ${config.mcpHost}:${port}`);
} else {
  for (const pid of pids) killTree(pid);
  console.log(`stopped MCP on ${config.mcpHost}:${port} (pid ${pids.join(", ")})`);
}
