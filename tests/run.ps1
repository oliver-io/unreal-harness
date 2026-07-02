# Run the UnrealMCP integration suite with zero setup.
#
# Uses `uv` (https://docs.astral.sh/uv — the test suite's one dependency; the
# /onboard skill installs it) to provide pytest in an ephemeral env, so there is
# nothing else to install. All args pass through to pytest, e.g.:
#
#   tests\run.ps1                       # headless (default)
#   tests\run.ps1 --ue-mode=gui         # real window + render tests
#   tests\run.ps1 --ue-attach -k smoke  # attach to a running editor, smoke only
#
# Requires UNREAL_ENGINE_ROOT to be set (unless --ue-attach). See tests/README.md.

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $env:UNREAL_ENGINE_ROOT -and ($args -notcontains "--ue-attach")) {
    Write-Warning "UNREAL_ENGINE_ROOT is not set — build/launch will fail."
    Write-Warning "Set it to your engine install root (e.g. `$env:UNREAL_ENGINE_ROOT='C:\path\to\UnrealEngine') or pass --ue-attach."
}

Push-Location $here
try {
    & uv run --with pytest --with "mcp[cli]>=1.10.0" python -m pytest @args
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
