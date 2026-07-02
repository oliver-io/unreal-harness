# Fetch non-bundled third-party UE plugins (the ones with public source) into a
# local, gitignored directory, so a project can reference them via the
# .uproject's AdditionalPluginDirectories.
#
# This is NOT for the UnrealMCP plugin -- that lives in this repo at src/Plugin
# and is referenced in place, never copied (see the onboard skill). It is also
# NOT for paid marketplace plugins (e.g. DragonIKPlugin): those have no public
# source to fetch -- install them through the Epic/Fab launcher into the engine.
#
#   scripts/install-plugins.ps1                      # fetch everything in the registry
#   scripts/install-plugins.ps1 GameLiftServerSDK    # fetch one by name
#   scripts/install-plugins.ps1 -List                # show the registry
#   scripts/install-plugins.ps1 -Force GameLiftServerSDK   # re-fetch over an existing copy
#
# Fetched into: <repo>/external/Plugins/<name>/   (gitignored)
# Then add "<...>/external/Plugins" to a project's AdditionalPluginDirectories
# (the bootstrap skill wires this for you).

param(
    [string[]]$Names,
    [switch]$List,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Dest = Join-Path $RepoRoot "external\Plugins"

# Registry of fetchable plugins. Add entries here as needed.
#   Name   -- the plugin name as referenced in a .uproject's "Plugins" list
#   Kind   -- "git" (clone Source) for now
#   Source -- public repository URL
#   Notes  -- any required post-fetch step (e.g. a native SDK build)
$Registry = @(
    [pscustomobject]@{
        Name   = "GameLiftServerSDK"
        Kind   = "git"
        Source = "https://github.com/amazon-gamelift/amazon-gamelift-plugin-unreal.git"
        Notes  = "Amazon GameLift plugin for Unreal (open source, not engine-bundled). The native C++ Server SDK must be built per the upstream README before the plugin compiles. The repo contains multiple .uplugin files (server SDK + editor UI + samples) -- point AdditionalPluginDirectories at the GameLiftServerSDK subfolder, or prune the rest, if you only want the server SDK. Docs: https://github.com/amazon-gamelift/amazon-gamelift-plugin-unreal"
    }
)

if ($List) {
    Write-Host "Registered fetchable plugins:"
    foreach ($e in $Registry) {
        Write-Host ("  {0,-22} {1}" -f $e.Name, $e.Source)
    }
    exit 0
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Host "error: 'git' not found on PATH -- required to fetch plugin sources."; exit 1
}

$selected = if ($Names) {
    $Names | ForEach-Object {
        $n = $_
        $hit = $Registry | Where-Object { $_.Name -ieq $n }
        if (-not $hit) { Write-Host "error: '$n' is not in the registry (see -List)."; exit 1 }
        $hit
    }
} else { $Registry }

New-Item -ItemType Directory -Force -Path $Dest | Out-Null

foreach ($e in $selected) {
    $target = Join-Path $Dest $e.Name
    if (Test-Path $target) {
        if ($Force) {
            Write-Host "=== $($e.Name): removing existing copy (-Force) ==="
            Remove-Item -Recurse -Force $target
        } else {
            Write-Host "=== $($e.Name): already present at $target (use -Force to re-fetch) ==="
            if ($e.Notes) { Write-Host "    note: $($e.Notes)" }
            continue
        }
    }
    Write-Host "=== Fetching $($e.Name) from $($e.Source) ==="
    & git clone --depth 1 $e.Source $target
    if ($LASTEXITCODE -ne 0) { Write-Host "error: git clone failed for $($e.Name)"; exit 1 }
    Write-Host "    -> $target"
    if ($e.Notes) { Write-Host "    note: $($e.Notes)" }
}

Write-Host "=== Done. Reference these via a project's AdditionalPluginDirectories (e.g. the repo's external/Plugins). ==="
