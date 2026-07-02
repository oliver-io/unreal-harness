# Discover Unreal Engine installs on this machine and emit ranked candidates as
# JSON on stdout. Used by the `onboard` skill to locate the engine source/install
# that UNREAL_ENGINE_ROOT should point at (the directory containing Engine/).
#
# Each candidate object has:
#   path      normalized absolute engine root (contains Engine/)
#   version   "Major.Minor.Patch" parsed from Engine/Build/Build.version, or ""
#   kind      "source"     - built from GitHub source (no InstalledBuild.txt)
#             "installed"  - Epic Games Launcher / binary build
#   hasEditor whether Engine/Binaries/Win64/UnrealEditor.exe exists (is it built?)
#   sources   where it was discovered: env | registry:source | registry:installed
#             | filesystem | sibling
#
# Ranking (best first): env-provided, then built source builds, then source
# builds, then built installed builds, then the rest.
#
# Discovery only; this script changes nothing. Output is JSON even when empty ([]).

$ErrorActionPreference = 'SilentlyContinue'

function Test-EngineRoot {
    # Returns a candidate hashtable if $Path looks like an engine root, else $null.
    param([string]$Path, [string]$Source)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    $p = $Path.Trim().Trim('"').TrimEnd('\', '/')
    if ([string]::IsNullOrWhiteSpace($p) -or -not (Test-Path -LiteralPath $p -PathType Container)) { return $null }
    try { $p = (Resolve-Path -LiteralPath $p -ErrorAction Stop).Path } catch { return $null }
    if (-not (Test-Path -LiteralPath (Join-Path $p 'Engine') -PathType Container)) { return $null }

    $version = ''
    $bv = Join-Path $p 'Engine\Build\Build.version'
    if (Test-Path -LiteralPath $bv) {
        try {
            $j = Get-Content -LiteralPath $bv -Raw | ConvertFrom-Json
            $version = "$($j.MajorVersion).$($j.MinorVersion).$($j.PatchVersion)"
        } catch {}
    }
    $installed = Test-Path -LiteralPath (Join-Path $p 'Engine\Build\InstalledBuild.txt')
    $hasEditor = Test-Path -LiteralPath (Join-Path $p 'Engine\Binaries\Win64\UnrealEditor.exe')

    return @{
        path      = $p
        version   = $version
        kind      = if ($installed) { 'installed' } else { 'source' }
        hasEditor = [bool]$hasEditor
        sources   = @($Source)
    }
}

$found = @()
function Add-Candidate {
    param([string]$Path, [string]$Source)
    $c = Test-EngineRoot -Path $Path -Source $Source
    if ($null -eq $c) { return }
    $existing = $found | Where-Object { $_.path -ieq $c.path } | Select-Object -First 1
    if ($existing) {
        if ($existing.sources -notcontains $Source) { $existing.sources += $Source }
    } else {
        $script:found += $c
    }
}

# 1. Already-configured env var -- highest trust.
Add-Candidate $env:UNREAL_ENGINE_ROOT 'env'

# 2. Registry: GitHub source builds register here (GUID -> path).
$buildsKey = 'HKCU:\Software\Epic Games\Unreal Engine\Builds'
if (Test-Path $buildsKey) {
    foreach ($name in (Get-Item $buildsKey).Property) {
        Add-Candidate ((Get-ItemProperty $buildsKey).$name) 'registry:source'
    }
}

# 3. Registry: Launcher / installed builds.
foreach ($root in @('HKLM:\SOFTWARE\EpicGames\Unreal Engine',
                     'HKLM:\SOFTWARE\WOW6432Node\EpicGames\Unreal Engine')) {
    Get-ChildItem $root | ForEach-Object {
        Add-Candidate (Get-ItemProperty $_.PSPath).InstalledDirectory 'registry:installed'
    }
}

# 4. Filesystem: conventional install locations (bounded globs, no deep scan).
$globs = @(
    "$env:ProgramFiles\Epic Games\UE_*",
    "$env:SystemDrive\Program Files\Epic Games\UE_*",
    "$env:SystemDrive\UE*\*",
    "$env:SystemDrive\UnrealEngine*",
    "$env:SystemDrive\Epic*\UE_*"
)
foreach ($g in $globs) {
    Get-ChildItem -Path $g -Directory | ForEach-Object { Add-Candidate $_.FullName 'filesystem' }
}

# 5. Sibling projects: a co-located .uproject's build/launch scripts often hardcode
#    the engine path (the only reliable pointer when EngineAssociation is a bare
#    version string). Scan one level of siblings of the repo root.
$repoRoot = Split-Path -Parent $PSScriptRoot
$parent = Split-Path -Parent $repoRoot
$rx = [regex]'(?i)([A-Za-z]:[\\/](?:[^"''<>|\r\n]+?))[\\/]Engine[\\/]Build[\\/]BatchFiles'
Get-ChildItem -Path $parent -Directory | Where-Object { $_.FullName -ine $repoRoot } | ForEach-Object {
    $dir = $_.FullName
    if (-not (Get-ChildItem -Path $dir -Filter *.uproject -File)) { return }
    Get-ChildItem -Path $dir -Include *.sh, *.bat, *.ps1 -File -Recurse -Depth 1 | ForEach-Object {
        foreach ($m in $rx.Matches((Get-Content -LiteralPath $_.FullName -Raw))) {
            Add-Candidate ($m.Groups[1].Value -replace '\\\\', '\') 'sibling'
        }
    }
}

# Rank and emit.
function Rank($c) {
    if ($c.sources -contains 'env')                 { return 0 }
    if ($c.kind -eq 'source'    -and $c.hasEditor)  { return 1 }
    if ($c.kind -eq 'source')                       { return 2 }
    if ($c.kind -eq 'installed' -and $c.hasEditor)  { return 3 }
    return 4
}
$ranked = $found | Sort-Object @{ Expression = { Rank $_ } }, @{ Expression = { $_.version }; Descending = $true }
ConvertTo-Json -InputObject @($ranked) -Depth 4
