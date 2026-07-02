#!/usr/bin/env bash
# Launch the Unreal Editor for a project (generic; reads UNREAL_ENGINE_ROOT +
# UNREAL_PROJECT_ROOT). Thin wrapper over launch-editor.ps1 — UE is Windows-only.
# All args pass through, e.g.:
#   scripts/launch-editor.sh
#   scripts/launch-editor.sh -Headless
#   scripts/launch-editor.sh -Project /c/path/to/MyGame
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PS1="${DIR}/launch-editor.ps1"

if command -v pwsh >/dev/null 2>&1; then PSH=pwsh; else PSH=powershell.exe; fi
if command -v cygpath >/dev/null 2>&1; then PS1="$(cygpath -w "$PS1")"; fi

exec "$PSH" -NoProfile -ExecutionPolicy Bypass -File "$PS1" "$@"
