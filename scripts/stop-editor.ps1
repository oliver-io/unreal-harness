# Stop the Unreal Editor and its helper processes. Generic -- no project hardcoded.
#
# Kills image names unique to Unreal Engine unconditionally. Image names that
# other apps also use (EpicWebHelper, crashpad_handler) are killed ONLY when the
# executable lives under UNREAL_ENGINE_ROOT, so this never whacks Chrome /
# Discord / the Epic Games Launcher.
#
# The MCP server (scripts/run-server) is INTENTIONALLY left running -- it's a
# long-lived listener on 127.0.0.1:8765 that survives editor restarts by design.
# Stop it explicitly with scripts/stop-server.ps1.
#
#   scripts/stop-editor.ps1
#
# Env:
#   UNREAL_ENGINE_ROOT   engine root; used to path-filter shared image names. If
#                        unset, shared images are left alone (only UE-unique
#                        images are killed).

$ErrorActionPreference = "Stop"

$uniqueImages = @(
    "UnrealEditor", "UnrealEditor-Cmd",
    "CrashReportClient", "CrashReportClientEditor",
    "ShaderCompileWorker", "UnrealLightmass", "InterchangeWorker"
)
$sharedImages = @("EpicWebHelper", "crashpad_handler")

$engine = $env:UNREAL_ENGINE_ROOT
$killed = 0

Write-Host "=== Stopping Unreal Engine processes ==="

foreach ($name in $uniqueImages) {
    Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "  stopping $($_.ProcessName) (pid $($_.Id))"
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
        $killed++
    }
}

if ($engine) {
    foreach ($name in $sharedImages) {
        Get-Process -Name $name -ErrorAction SilentlyContinue | Where-Object {
            try { $_.Path -and $_.Path.StartsWith($engine, [System.StringComparison]::OrdinalIgnoreCase) }
            catch { $false }
        } | ForEach-Object {
            Write-Host "  stopping $($_.ProcessName) (pid $($_.Id), UE-owned)"
            Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
            $killed++
        }
    }
}

if ($killed -eq 0) { Write-Host "=== No Unreal Engine processes found ===" }
else { Write-Host "=== Stopped $killed process(es); MCP server left running ===" }
