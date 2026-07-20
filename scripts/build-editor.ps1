# Build an Unreal project's editor target. Generic -- no project is hardcoded.
#
# Resolves the engine from UNREAL_ENGINE_ROOT and the project from
# UNREAL_PROJECT_ROOT (or -Project). Builds <ProjectName>Editor by default.
# Building compiles the project module plus every enabled plugin (including the
# UnrealMCP plugin loaded from this repo), which is what makes the C++ loadable.
#
#   scripts/build-editor.ps1
#   scripts/build-editor.ps1 -Project C:\path\to\MyGame
#   scripts/build-editor.ps1 -Target MyGame -Configuration Shipping
#
# Env:
#   UNREAL_ENGINE_ROOT   engine root (dir containing Engine/)              [required]
#   UNREAL_PROJECT_ROOT  project root (dir containing the .uproject)       [required unless -Project]
# Set these via the `onboard` skill.

param(
    [string]$Project = $env:UNREAL_PROJECT_ROOT,
    [string]$Target,
    [string]$Configuration = "Development",
    # UBA (Unreal Build Accelerator) kills compile workers when system COMMIT charge
    # nears the commit limit ("Killed process ... Low on memory (131.2gb/137.4gb),
    # kill threshold 130.5gb"). NOTE (corrected 2026-07-20): that check is CORRECT,
    # not buggy — this box has ~32 GB RAM plus a large pagefile, and a leaked bun MCP
    # server holding 58 GB of COMMIT (with a 60 MB working set, so it hides from any
    # RAM/working-set check) really had it at the limit. Bouncing the server dropped
    # commit 126 GB -> 68 GB. So: if UBA starts killing workers, FIRST check
    # `Get-Process bun | Select PrivateMemorySize64` and bounce run-server.ps1 —
    # do not assume UBA is lying. -NoUBA stays the default because UBA's parallelism
    # is a poor fit for 32 GB regardless; pass -UseUBA to opt back in.
    [switch]$UseUBA
)

$ErrorActionPreference = "Stop"

function Resolve-UProject([string]$p) {
    if (-not $p) { Write-Host "error: no project -- set UNREAL_PROJECT_ROOT or pass -Project."; exit 1 }
    if ($p -like "*.uproject") {
        if (-not (Test-Path $p)) { Write-Host "error: uproject not found: $p"; exit 1 }
        return (Resolve-Path $p).Path
    }
    if (-not (Test-Path $p)) { Write-Host "error: project path not found: $p"; exit 1 }
    $found = @(Get-ChildItem -Path $p -Filter *.uproject -File)
    if ($found.Count -eq 0) { Write-Host "error: no .uproject in $p"; exit 1 }
    if ($found.Count -gt 1) { Write-Host "error: multiple .uproject in $p -- pass -Project <file>"; exit 1 }
    return $found[0].FullName
}

$engine = $env:UNREAL_ENGINE_ROOT
if (-not $engine) { Write-Host "error: UNREAL_ENGINE_ROOT is not set -- run the onboard skill."; exit 1 }
if (-not (Test-Path (Join-Path $engine "Engine"))) {
    Write-Host "error: UNREAL_ENGINE_ROOT=$engine has no Engine/ -- not an engine root."; exit 1
}

$uproject = Resolve-UProject $Project
$projName = [IO.Path]::GetFileNameWithoutExtension($uproject)
if (-not $Target) { $Target = "${projName}Editor" }

$buildBat = Join-Path $engine "Engine\Build\BatchFiles\Build.bat"
if (-not (Test-Path $buildBat)) { Write-Host "error: Build.bat not found at $buildBat"; exit 1 }

# GAP-058: UBT outputs the modules of an "external" plugin (one referenced via
# AdditionalPluginDirectories, i.e. NOT under the project's own Plugins/ folder) into the PROJECT's
# Binaries, but the editor loads that plugin from the plugin's OWN Binaries. So a full rebuild never
# reaches a freshly-launched editor -- only live-coding (in-memory patch) does. After a good build we
# sync each freshly-built plugin module DLL+PDB from the project Binaries into the plugin's Binaries
# and stamp the plugin's .modules with the project's BuildId so the editor accepts them. This makes
# `build-editor.ps1` + relaunch actually load the new code, matching the live-coding path.
function Sync-ExternalPluginBinaries([string]$Uproject) {
    try {
        $projBin = Join-Path (Split-Path $Uproject -Parent) "Binaries\Win64"
        $projModules = Join-Path $projBin "UnrealEditor.modules"
        $pluginRoot = Join-Path $PSScriptRoot "..\src\Plugin"
        if (-not (Test-Path $projModules) -or -not (Test-Path $pluginRoot)) { return }
        $projBuildId = ([regex]::Match((Get-Content $projModules -Raw), '"BuildId"\s*:\s*"([^"]+)"')).Groups[1].Value
        foreach ($pdir in (Get-ChildItem -Path $pluginRoot -Directory)) {
            $pBin = Join-Path $pdir.FullName "Binaries\Win64"
            $pModules = Join-Path $pBin "UnrealEditor.modules"
            if (-not (Test-Path $pModules)) { continue }   # plugin has never been built standalone -> nothing to sync
            $synced = @()
            foreach ($mp in [regex]::Matches((Get-Content $pModules -Raw), '"[^"]+"\s*:\s*"([^"]+\.dll)"')) {
                $dll = $mp.Groups[1].Value
                $src = Join-Path $projBin $dll
                if (-not (Test-Path $src)) { continue }    # UBT didn't put this module in the project Binaries -> leave plugin copy
                Copy-Item $src (Join-Path $pBin $dll) -Force
                $pdb = [IO.Path]::ChangeExtension($dll, ".pdb")
                if (Test-Path (Join-Path $projBin $pdb)) { Copy-Item (Join-Path $projBin $pdb) (Join-Path $pBin $pdb) -Force }
                $synced += $dll
            }
            if ($synced.Count -gt 0 -and $projBuildId) {
                (Get-Content $pModules -Raw) -replace '("BuildId"\s*:\s*")[^"]+(")', ('${1}' + $projBuildId + '${2}') |
                    Set-Content $pModules -NoNewline -Encoding utf8
                Write-Host "    plugin-sync ($($pdir.Name)): $($synced -join ', ')  [BuildId $projBuildId]"
            }
        }
    } catch {
        Write-Host "    plugin-sync WARNING: $($_.Exception.Message)"  # never fail the build over a sync hiccup
    }
}

# Coordinate with other agents: refuse if a build is already running, register
# ours (with this script's PID) otherwise. Released in finally -- even on failure.
. (Join-Path $PSScriptRoot "build-coord.ps1")
$label = if ($env:CLAUDE_BUILD_LABEL) { $env:CLAUDE_BUILD_LABEL } else { "build-editor:$Target" }
$buildId = Enter-BuildLock -Target "$Target Win64 $Configuration" -Label $label

try {
    Write-Host "=== Building $Target ($Configuration, Win64) ==="
    Write-Host "    project: $uproject"
    $extraArgs = @()
    if (-not $UseUBA) { $extraArgs += "-NoUBA" }
    & $buildBat $Target Win64 $Configuration "-Project=$uproject" -WaitMutex @extraArgs
    $code = $LASTEXITCODE
    if ($code -eq 0) {
        Write-Host "=== Build succeeded ==="
        Sync-ExternalPluginBinaries -Uproject $uproject
    }
    else { Write-Host "=== Build FAILED (exit $code) ===" }
} finally {
    Exit-BuildLock -BuildId $buildId
}
exit $code
