# Launch the Unreal Editor for a project. Generic - no project is hardcoded.
#
# Resolves the engine from UNREAL_ENGINE_ROOT and the project from
# UNREAL_PROJECT_ROOT (or -Project). Opens the GUI editor by default; pass
# -Headless to boot UnrealEditor-Cmd with -nullrhi (full editor boot, no GPU -
# the MCP bridge still comes up, so this is the mode for unattended MCP driving).
#
# Launches with -AutoDeclinePackageRecovery so an unclean prior shutdown doesn't
# block startup on the modal "Restore Packages" dialog. Note: that DISCARDS
# unsaved auto-save recovery data (stock UE has no no-prompt restore path).
#
#   scripts/launch-editor.ps1
#   scripts/launch-editor.ps1 -Headless
#   scripts/launch-editor.ps1 -Project C:\path\to\MyGame
#
# Env:
#   UNREAL_ENGINE_ROOT   engine root (dir containing Engine/)            [required]
#   UNREAL_PROJECT_ROOT  project root (dir containing the .uproject)     [required unless -Project]

param(
    [string]$Project = $env:UNREAL_PROJECT_ROOT,
    [switch]$Headless
)

$ErrorActionPreference = "Stop"

function Resolve-UProject([string]$p) {
    if (-not $p) { Write-Host "error: no project - set UNREAL_PROJECT_ROOT or pass -Project."; exit 1 }
    if ($p -like "*.uproject") {
        if (-not (Test-Path $p)) { Write-Host "error: uproject not found: $p"; exit 1 }
        return (Resolve-Path $p).Path
    }
    if (-not (Test-Path $p)) { Write-Host "error: project path not found: $p"; exit 1 }
    $found = @(Get-ChildItem -Path $p -Filter *.uproject -File)
    if ($found.Count -eq 0) { Write-Host "error: no .uproject in $p"; exit 1 }
    if ($found.Count -gt 1) { Write-Host "error: multiple .uproject in $p - pass -Project <file>"; exit 1 }
    return $found[0].FullName
}

$engine = $env:UNREAL_ENGINE_ROOT
if (-not $engine) { Write-Host "error: UNREAL_ENGINE_ROOT is not set - run the onboard skill."; exit 1 }
if (-not (Test-Path (Join-Path $engine "Engine"))) {
    Write-Host "error: UNREAL_ENGINE_ROOT=$engine has no Engine/ - not an engine root."; exit 1
}

$uproject = Resolve-UProject $Project
$binDir = Join-Path $engine "Engine\Binaries\Win64"
$logDir = Join-Path (Split-Path $uproject -Parent) "Saved\Logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$common = @("-AutoDeclinePackageRecovery")
if ($Headless) {
    $exe = Join-Path $binDir "UnrealEditor-Cmd.exe"
    $launchArgs = @($uproject, "-nullrhi", "-nosound", "-unattended", "-nopause", "-nosplash") + $common
    $logFile = Join-Path $logDir "editor-headless.log"
} else {
    $exe = Join-Path $binDir "UnrealEditor.exe"
    $launchArgs = @($uproject) + $common
    $logFile = Join-Path $logDir "editor-gui.log"
}
if (-not (Test-Path $exe)) {
    Write-Host "error: editor binary not found: $exe (build the engine/editor target first - scripts/build-editor.ps1)"; exit 1
}

$launchArgs += @("-stdout", "-FullStdOutLogOutput", "-AbsLog=$logFile")

Write-Host "=== Launching $(Split-Path $exe -Leaf) ==="
Write-Host "    project: $uproject"
$proc = Start-Process -FilePath $exe -ArgumentList $launchArgs -PassThru
Write-Host "    pid $($proc.Id); log: $logFile"
Write-Host "    The MCP bridge accepts commands once the editor finishes booting (poll mcp_status)."
