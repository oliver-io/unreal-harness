#!/usr/bin/env bash
# google-key.sh — resolve / store the Google AI Studio (Gemini) API key used by
# the host-side visual-judgment skill (`/visual-critique`).
# The key is needed ONLY for those Gemini calls — the editor/MCP server never uses
# it. This is an OPT-IN feature; skipping it just means that skill is
# unavailable.
#
# Storage: a gitignored `.env` at the repo root (`GOOGLE_STUDIO_API_KEY=...`).
# critique.ts picks it up automatically — Bun auto-loads the repo
# `.env`, and the script also walks parent dirs for it — so no shell restart.
#
#   scripts/google-key.sh check          # exit 0 if a key is resolvable, else 1
#   scripts/google-key.sh set            # read a key from stdin and upsert .env
#   echo "AIza..." | scripts/google-key.sh set
#
# Reading the key from stdin (not an argv) keeps it out of shell history / the
# process list.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="$ROOT/.env"

# Echo the GOOGLE_STUDIO_API_KEY value found in .env (last wins), unquoted; empty if none.
key_from_env_file() {
	[ -f "$ENV_FILE" ] || return 0
	grep -E '^[[:space:]]*GOOGLE_STUDIO_API_KEY[[:space:]]*=' "$ENV_FILE" 2>/dev/null \
		| tail -n1 \
		| sed -E 's/^[^=]*=[[:space:]]*//; s/^["'"'"']//; s/["'"'"']$//'
}

cmd="${1:-check}"
case "$cmd" in
	check)
		if [ -n "${GOOGLE_STUDIO_API_KEY:-}" ]; then
			echo "GOOGLE_STUDIO_API_KEY: set (environment)"; exit 0
		fi
		if [ -n "$(key_from_env_file)" ]; then
			echo "GOOGLE_STUDIO_API_KEY: set (.env)"; exit 0
		fi
		echo "GOOGLE_STUDIO_API_KEY: NOT set (no env var, none in $ENV_FILE)"; exit 1
		;;
	set)
		# One line from stdin = the key. Trim surrounding whitespace.
		IFS= read -r key || true
		key="$(printf '%s' "$key" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')"
		if [ -z "$key" ]; then
			echo "no key on stdin; nothing written" >&2; exit 1
		fi
		# Safety: never let the secret be committed. If .env is tracked (gitignore
		# doesn't apply to already-tracked files), untrack it first.
		if git -C "$ROOT" ls-files --error-unmatch .env >/dev/null 2>&1; then
			git -C "$ROOT" rm --cached --sparse .env >/dev/null 2>&1 \
				|| git -C "$ROOT" rm --cached .env >/dev/null 2>&1 || true
			echo "note: untracked .env from git so the key is never committed (.gitignore covers it)." >&2
		fi
		touch "$ENV_FILE"; chmod 600 "$ENV_FILE" 2>/dev/null || true
		# Upsert: drop any existing GOOGLE_STUDIO_API_KEY line, then append the new one.
		if grep -qE '^[[:space:]]*GOOGLE_STUDIO_API_KEY[[:space:]]*=' "$ENV_FILE" 2>/dev/null; then
			tmp="$(mktemp)"
			grep -vE '^[[:space:]]*GOOGLE_STUDIO_API_KEY[[:space:]]*=' "$ENV_FILE" > "$tmp" || true
			mv "$tmp" "$ENV_FILE"
		fi
		printf 'GOOGLE_STUDIO_API_KEY=%s\n' "$key" >> "$ENV_FILE"
		echo "Stored GOOGLE_STUDIO_API_KEY in $ENV_FILE (gitignored). visual-critique will pick it up automatically."
		;;
	*)
		echo "usage: google-key.sh [check|set]" >&2; exit 2
		;;
esac
