#!/usr/bin/env bash
# Stop the MCP server: kill whatever is listening on 127.0.0.1:8765.
# Generic kill-by-port - safe to run when nothing is listening (no-op).

set -e

if command -v powershell.exe >/dev/null 2>&1; then
  powershell.exe -NoProfile -Command "
    \$conns = Get-NetTCPConnection -LocalPort 8765 -State Listen -ErrorAction SilentlyContinue
    if (-not \$conns) { Write-Host 'nothing listening on 8765'; exit 0 }
    \$conns | Select-Object -ExpandProperty OwningProcess -Unique | ForEach-Object {
      Write-Host ('stopping pid ' + \$_)
      Stop-Process -Id \$_ -Force -ErrorAction SilentlyContinue
    }
  "
elif command -v lsof >/dev/null 2>&1; then
  PIDS=$(lsof -tiTCP:8765 -sTCP:LISTEN || true)
  if [[ -z "${PIDS}" ]]; then
    echo "nothing listening on 8765"
  else
    echo "stopping pid(s): ${PIDS}"
    kill ${PIDS}
  fi
else
  echo "error: need powershell.exe (Windows) or lsof (POSIX) to find the listener" >&2
  exit 1
fi