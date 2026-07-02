#!/usr/bin/env bash
# Fetch non-bundled third-party UE plugins (with public source) into a local,
# gitignored directory. Thin wrapper over install-plugins.ps1. All args pass
# through:
#   scripts/install-plugins.sh                    # fetch everything
#   scripts/install-plugins.sh GameLiftServerSDK  # fetch one
#   scripts/install-plugins.sh -List
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PS1="${DIR}/install-plugins.ps1"

if command -v pwsh >/dev/null 2>&1; then PSH=pwsh; else PSH=powershell.exe; fi
if command -v cygpath >/dev/null 2>&1; then PS1="$(cygpath -w "$PS1")"; fi

exec "$PSH" -NoProfile -ExecutionPolicy Bypass -File "$PS1" "$@"
