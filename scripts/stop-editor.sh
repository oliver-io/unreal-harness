#!/usr/bin/env bash
# Stop the Unreal Editor and its helper processes (generic). Thin wrapper over
# stop-editor.ps1 — UE is Windows-only. Leaves the MCP server running.
#   scripts/stop-editor.sh
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PS1="${DIR}/stop-editor.ps1"

if command -v pwsh >/dev/null 2>&1; then PSH=pwsh; else PSH=powershell.exe; fi
if command -v cygpath >/dev/null 2>&1; then PS1="$(cygpath -w "$PS1")"; fi

exec "$PSH" -NoProfile -ExecutionPolicy Bypass -File "$PS1" "$@"
