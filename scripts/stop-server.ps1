# Stop the MCP server: kill whatever is listening on 127.0.0.1:8765.
# Generic kill-by-port - safe to run when nothing is listening (no-op).

$ErrorActionPreference = "Stop"

$conns = Get-NetTCPConnection -LocalPort 8765 -State Listen -ErrorAction SilentlyContinue
if (-not $conns) {
    Write-Host "nothing listening on 8765"
    exit 0
}
$conns | Select-Object -ExpandProperty OwningProcess -Unique | ForEach-Object {
    Write-Host "stopping pid $_"
    Stop-Process -Id $_ -Force -ErrorAction SilentlyContinue
}