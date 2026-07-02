#!/usr/bin/env python3
"""PreToolUse(Bash) gate for AI art generation.

When a host-side art-generation command is about to run (the icon skill's iconify.py;
AI-generated keycaps for key-indicator-helper) but no OpenAI key is configured, DENY the
call with an actionable reason so it never fails with an ugly KeyError. Claude reads the
reason, sets the key up once, then retries.

Gating the generation COMMAND (not the Skill tool) means it fires no matter how the icon
skill was entered (`/icon` slash command or otherwise) and never misfires when the icon
skill is used only to import/wire an existing texture.

Reads the PreToolUse event JSON on stdin; prints a hook decision JSON (or nothing).
"""
import json
import os
import re
import sys
from pathlib import Path

# A genuine INVOCATION of iconify.py: an interpreter (python/python3/py), then a bare
# script path token ending in iconify.py — with no quote in between, so a mere REFERENCE
# (e.g. `python -c "...open('.../iconify.py')"`, `cat .../iconify.py`, `grep iconify.py`)
# does NOT match. The script path may use / or \ separators.
INVOKE_RE = re.compile(
    r"""(?:^|[\s;&|(])          # start or a shell separator
        (?:python3?|py)\b       # the interpreter
        [^"';|&]*?              # flags/paths, but no quote/separator (rules out -c "...")
        (?:^|[\s/\\])           # a path boundary right before the script name
        iconify\.py             # the generation script
        (?=\s|$)                # followed by an arg or end (not e.g. iconify.pyc)
    """,
    re.VERBOSE,
)


def key_configured(root: Path) -> bool:
    """True if OPENAI_API_KEY is resolvable: an env var, or in the repo's .env."""
    if os.environ.get("OPENAI_API_KEY"):
        return True
    env_file = root / ".env"
    try:
        for line in env_file.read_text().splitlines():
            s = line.strip()
            if s.startswith("OPENAI_API_KEY") and "=" in s:
                val = s.split("=", 1)[1].strip().strip("\"'")
                if val:
                    return True
    except FileNotFoundError:
        pass
    return False


def main() -> int:
    try:
        event = json.load(sys.stdin)
    except Exception:
        return 0  # not parseable — stay out of the way

    if event.get("tool_name") != "Bash":
        return 0
    cmd = event.get("tool_input", {}).get("command", "")

    if not INVOKE_RE.search(cmd):
        return 0
    if "--skip-gen" in cmd:
        return 0  # downscale-only: no API call, no key needed

    root = Path(os.environ.get("CLAUDE_PROJECT_DIR") or Path(__file__).resolve().parents[2])
    if key_configured(root):
        return 0

    reason = (
        "AI image generation needs an OpenAI API key, which is not configured (no "
        "OPENAI_API_KEY env var and none in the gitignored .env). This is a ONE-TIME "
        "setup. Ask the user for their OpenAI API key, then store it WITHOUT echoing it "
        "into the transcript: pipe it into the shared helper — "
        "`printf %s '<key>' | scripts/openai-key.sh set` (or have the user run that "
        "themselves). It writes the key to the gitignored .env; iconify.py picks it up "
        "automatically with no restart. Then re-run this command. If the user does not "
        "have / does not want to use an OpenAI key, AI art generation is unavailable — "
        "use existing art with --skip-gen, or supply the texture another way."
    )
    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": reason,
        }
    }))
    return 0


if __name__ == "__main__":
    sys.exit(main())
