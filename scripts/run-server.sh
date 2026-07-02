#!/usr/bin/env bash
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

set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v bun >/dev/null 2>&1; then
  echo "error: 'bun' not found on PATH - install from https://bun.sh" >&2
  exit 1
fi

# Refuse to double-bind: a server is already listening on 8765.
if command -v powershell.exe >/dev/null 2>&1; then
  if powershell.exe -NoProfile -Command "if (Get-NetTCPConnection -LocalPort 8765 -State Listen -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 }" 2>/dev/null; then
    echo "error: something is already listening on 127.0.0.1:8765 - stop it first (scripts/stop-server.sh)" >&2
    exit 1
  fi
elif command -v lsof >/dev/null 2>&1; then
  if lsof -iTCP:8765 -sTCP:LISTEN >/dev/null 2>&1; then
    echo "error: something is already listening on 127.0.0.1:8765 - stop it first (scripts/stop-server.sh)" >&2
    exit 1
  fi
fi

cd "${REPO_ROOT}"
[ -d node_modules ] || bun install
exec bun run mcp "$@"
