#!/usr/bin/env bash
# Run the UnrealMCP integration suite with zero setup.
#
# Uses `uv` (https://docs.astral.sh/uv — the test suite's one dependency; the
# /onboard skill installs it) to provide pytest in an ephemeral env, so there is
# nothing else to install. All args pass through to pytest, e.g.:
#
#   tests/run.sh                        # headless (default)
#   tests/run.sh --ue-mode=gui          # real window + render tests
#   tests/run.sh --ue-attach -k smoke   # attach to a running editor, smoke only
#
# Requires UNREAL_ENGINE_ROOT to be set (unless --ue-attach). See tests/README.md.

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -z "${UNREAL_ENGINE_ROOT:-}" ]]; then
  case " $* " in
    *" --ue-attach "*) : ;;
    *) echo "warning: UNREAL_ENGINE_ROOT is not set — build/launch will fail. Set it to your engine install root (the dir containing Engine/) or pass --ue-attach." >&2 ;;
  esac
fi

cd "${HERE}"
exec uv run --with pytest --with "mcp[cli]>=1.10.0" python -m pytest "$@"
