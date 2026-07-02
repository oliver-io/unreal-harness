#!/usr/bin/env bash
# Build an Unreal project's editor target (generic; reads UNREAL_ENGINE_ROOT +
# UNREAL_PROJECT_ROOT). Thin wrapper over build-editor.ps1 — UE builds are
# Windows-only. All args pass through, e.g.:
#   scripts/build-editor.sh
#   scripts/build-editor.sh -Project /c/path/to/MyGame
#   scripts/build-editor.sh -Target MyGame -Configuration Shipping
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PS1="${DIR}/build-editor.ps1"

if command -v pwsh >/dev/null 2>&1; then PSH=pwsh; else PSH=powershell.exe; fi
if command -v cygpath >/dev/null 2>&1; then PS1="$(cygpath -w "$PS1")"; fi

exec "$PSH" -NoProfile -ExecutionPolicy Bypass -File "$PS1" "$@"
