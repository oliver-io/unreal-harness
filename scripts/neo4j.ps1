# Thin wrapper for the generic Unreal -> Neo4j ingester (plugin/unreal-skills/skills/neo4j/tool). Forwards all
# args to its CLI. Runs `bun install` once if deps are missing. Generic — no project
# hardcoded; scope with the CLI's selector flags. See plugin/unreal-skills/skills/neo4j/tool/README.md.
#
#   scripts/neo4j.ps1 discover
#   scripts/neo4j.ps1 ingest --class StateTree
#   scripts/neo4j.ps1 query "MATCH (n) RETURN labels(n)[0], count(*)"
#
# Env (see README): MCP_URL, NEO4J_URI, NEO4J_USER, NEO4J_PASS (required for graph ops).

$ErrorActionPreference = "Stop"
$tool = Join-Path (Split-Path -Parent $PSScriptRoot) "plugin\unreal-skills\skills\neo4j\tool"
if (-not (Test-Path (Join-Path $tool "node_modules"))) {
    Write-Host "=== installing deps (first run) ==="
    Push-Location $tool; try { bun install } finally { Pop-Location }
}
Push-Location $tool
try { bun run src/cli.ts @args; exit $LASTEXITCODE } finally { Pop-Location }
