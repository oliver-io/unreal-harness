# Boot the MCP server (Bun). Streamable-http on 127.0.0.1:8765.
#
# Requires Bun (https://bun.sh) on PATH; `bun install` runs automatically on
# first launch if dependencies are missing.
#
# Optional environment (consumed by project-coupled tools):
#   UNREAL_PROJECT_ROOT  Host UE project root (directory containing the
#                        .uproject). Required by editor_read_logs and
#                        editor_build_game_target.
#   UNREAL_ENGINE_ROOT   Engine install root (directory containing Engine/).
# Unset vars degrade gracefully: the affected tools return a structured
# error naming the variable; everything editor-connected works regardless.
#
# Server knobs (optional): UNREAL_MCP_PORT, UNREAL_MCP_SURFACE (full|compact|code),
# UNREAL_MCP_MAX_RESULT_BYTES. See src/server/README.md.

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot

if (-not (Get-Command bun -ErrorAction SilentlyContinue)) {
    Write-Error "'bun' not found on PATH - install from https://bun.sh"
    exit 1
}

# Refuse to double-bind: a server is already listening on 8765.
if (Get-NetTCPConnection -LocalPort 8765 -State Listen -ErrorAction SilentlyContinue) {
    Write-Error "something is already listening on 127.0.0.1:8765 - stop it first (scripts/stop-server.ps1)"
    exit 1
}

Set-Location $RepoRoot
if (-not (Test-Path (Join-Path $RepoRoot "node_modules"))) { & bun install }
& bun run mcp @Args
