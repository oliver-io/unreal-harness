# Build-lock coordination helpers -- dot-source from a build script:
#
#   . (Join-Path $PSScriptRoot "build-coord.ps1")
#   $buildId = Enter-BuildLock -Target $Target -Label "build-editor"
#   try { <run the build> } finally { Exit-BuildLock -BuildId $buildId }
#
# These call the always-on MCP server's /build REST endpoints so concurrent agents
# don't rebuild at once. The script sends its OWN $PID; if it crashes without
# releasing, the server reclaims the lock via a PID-liveness check (plus a TTL
# backstop). FAIL-OPEN: if the MCP server is unreachable, builds proceed anyway --
# coordination is a stability layer, never a hard gate on building.
#
# Works on Windows PowerShell 5.1 and PowerShell 7 (no 7-only syntax).

function Get-McpBase {
    $h = if ($env:UNREAL_MCP_HOST) { $env:UNREAL_MCP_HOST } else { "127.0.0.1" }
    $p = if ($env:UNREAL_MCP_PORT) { $env:UNREAL_MCP_PORT } else { "8765" }
    return "http://${h}:${p}"
}

# Acquire the build lock. Returns the build_id on success (pass to Exit-BuildLock),
# $null if coordination was unavailable (fail-open). EXITS the script (code 75,
# EX_TEMPFAIL) if another build holds the lock.
function Enter-BuildLock {
    param([string]$Target, [string]$Label = "build")

    $base = Get-McpBase
    $body = @{ pid = $PID; target = $Target; label = $Label; host = $env:COMPUTERNAME } | ConvertTo-Json -Compress
    try {
        $resp = Invoke-RestMethod -Uri "$base/build/acquire" -Method Post -Body $body `
            -ContentType "application/json" -TimeoutSec 5
    } catch {
        Write-Host "build-lock: MCP server not reachable at $base -- proceeding without coordination."
        return $null
    }

    if (-not $resp.ok) {
        $h = $resp.holder
        $ago = if ($h.held_ms) { [int]($h.held_ms / 1000) } else { 0 }
        Write-Host ""
        Write-Host "=== Sorry, you can't build right now -- someone else is building. ==="
        Write-Host "    held by : $($h.label) (pid $($h.pid))"
        if ($h.target) { Write-Host "    target  : $($h.target)" }
        Write-Host "    started : ${ago}s ago"
        Write-Host "    Wait for it to finish (check build_status via the MCP) and retry."
        Write-Host ""
        exit 75
    }

    Write-Host "build-lock: acquired (build_id $($resp.build.build_id))."
    return $resp.build.build_id
}

# Release the build lock. Safe to call with $null (no-op) and safe if the MCP is
# down (the lock then expires via the PID check / TTL). Call from a finally block.
function Exit-BuildLock {
    param([string]$BuildId)
    if (-not $BuildId) { return }

    $base = Get-McpBase
    $body = @{ build_id = $BuildId; pid = $PID } | ConvertTo-Json -Compress
    try {
        Invoke-RestMethod -Uri "$base/build/release" -Method Post -Body $body `
            -ContentType "application/json" -TimeoutSec 5 | Out-Null
        Write-Host "build-lock: released."
    } catch {
        Write-Host "build-lock: release failed (MCP down?) -- the lock will self-expire."
    }
}
