#!/usr/bin/env python3
"""PreToolUse(Bash) gate for the Gemini-backed visual-judgment skills.

When a command is about to run the host-side visual skill that calls Google AI
Studio (Gemini) — `/visual-critique` (critique.ts) — but no Google key is configured, DENY
the call with an actionable reason so it never fails with an ugly
"GOOGLE_STUDIO_API_KEY not found". Claude reads the reason, sets the key up
once (opt-in), then retries.

Gating the COMMAND (not the Skill tool) means it fires no matter how the skill was entered
(slash command or a direct `bun ...` invocation). Unlike the OpenAI/iconify path there is no
offline mode — this skill fundamentally needs the API — so there is no --skip bypass.

Reads the PreToolUse event JSON on stdin; prints a hook decision JSON (or nothing).
"""
import json
import os
import re
import sys
from pathlib import Path

# A genuine INVOCATION of critique.ts: the `bun` (or `bunx`) runtime, then a bare script-path
# token ending in critique.ts — with no quote in between, so a mere REFERENCE (e.g.
# `cat .../critique.ts`, `grep critique.ts`, `bun -e "...critique.ts"`) does NOT match. The
# script path may use / or \ separators.
INVOKE_RE = re.compile(
    r"""(?:^|[\s;&|(])          # start or a shell separator
        bunx?\b                 # the bun runtime (bun or bunx)
        [^"';|&]*?              # flags/paths, but no quote/separator (rules out -e "...")
        (?:^|[\s/\\])           # a path boundary right before the script name
        critique\.ts            # the Gemini-backed skill script
        (?=\s|$)                # followed by an arg or end
    """,
    re.VERBOSE,
)


def key_configured(root: Path) -> bool:
    """True if GOOGLE_STUDIO_API_KEY is resolvable: an env var, or in the repo's .env."""
    if os.environ.get("GOOGLE_STUDIO_API_KEY"):
        return True
    env_file = root / ".env"
    try:
        for line in env_file.read_text().splitlines():
            s = line.strip()
            if s.startswith("GOOGLE_STUDIO_API_KEY") and "=" in s:
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
    if re.search(r"(?:^|\s)(?:--help|-h)\b", cmd):
        return 0  # help text needs no key

    root = Path(os.environ.get("CLAUDE_PROJECT_DIR") or Path(__file__).resolve().parents[2])
    if key_configured(root):
        return 0

    reason = (
        "The visual-judgment skill (/visual-critique) calls Google AI "
        "Studio (Gemini) and needs a GOOGLE_STUDIO_API_KEY, which is not configured (no env "
        "var and none in the gitignored .env). This is a ONE-TIME, OPT-IN setup. Ask the "
        "user for their Google AI Studio API key (https://aistudio.google.com/apikey), then "
        "store it WITHOUT echoing it into the transcript: pipe it into the shared helper — "
        "`printf %s '<key>' | scripts/google-key.sh set` (or have the user run that "
        "themselves). It writes the key to the gitignored .env; critique.ts picks "
        "it up automatically (Bun auto-loads .env) with no restart. Then re-run this command. "
        "If the user does not have / does not want a Google key, this visual skill is "
        "unavailable — judge the render yourself or have the user eyeball it."
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
