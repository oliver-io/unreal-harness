"""Gemini-backed vision critic for perceptual integration tests (TASK-8).

A thin, stdlib-only (urllib) helper that asks the pinned Gemini model a NARROW,
binary-decidable question about a captured image and returns a machine-usable
verdict. This is the verifier that unlocks the "perceptual" test category in
docs/loops/skills/TASKS.md: arrange deterministically via typed primitives,
capture via a fixed rig, then validate a semantic claim here.

Request mechanics mirror `.claude/skills/visual-critique/critique.ts` (POST
generateContent, ``x-goog-api-key`` header, image as ``inline_data`` base64,
``responseMimeType: application/json``) — with two deliberate deviations for
tests: **temperature 0** and a **PINNED model with no fallback** (a silent
model swap would change what the oracle is).

Key resolution mirrors ``key_configured()`` in
`.claude/hooks/gemini-cred-gate.py` (env var, else the repo `.env`), extended
to accept BOTH spellings: ``GEMINI_API_KEY`` (server-preferred, see
src/server/src/config.ts) and ``GOOGLE_STUDIO_API_KEY`` (skills/hook
convention). The key is never printed.

Anti-rubber-stamp rule (binding on consumers): every perceptual assertion made
with :func:`ask` must ship a control/differential arm — a frame or question
whose expected answer is the OPPOSITE — so an always-agreeable critic fails
the test rather than passing it.
"""

from __future__ import annotations

import base64
import json
import os
import urllib.error
import urllib.request
from pathlib import Path
from typing import Sequence

import pytest

# Repo root (this file lives at tests/harness/vision_critic.py). Module-level so
# tests can monkeypatch it to point the .env lookup away from the real repo.
REPO_ROOT = Path(__file__).resolve().parents[2]

#: The one model perceptual tests are calibrated against. PINNED — no fallback.
DEFAULT_MODEL = "gemini-3.5-flash"

#: Accepted key variable names, in resolution order.
KEY_VARS = ("GEMINI_API_KEY", "GOOGLE_STUDIO_API_KEY")

_API_BASE = "https://generativelanguage.googleapis.com/v1beta/models"
_DEFAULT_TIMEOUT_S = 45.0


class VisionCriticError(RuntimeError):
    """Any failure talking to / interpreting the critic (HTTP, bad JSON, ...)."""


class MissingKeyError(VisionCriticError):
    """No Gemini API key resolvable — see :func:`gemini_key`."""


# ── key resolution ───────────────────────────────────────────────────────────

def gemini_key(root: Path | None = None) -> str | None:
    """Resolve the Gemini key: env vars first, then the repo ``.env``.

    Mirrors ``key_configured()`` in .claude/hooks/gemini-cred-gate.py, but
    returns the key (or None) and accepts both variable spellings. Exact
    name match only — ``GOOGLE_STUDIO_API_KEY_NAME=...`` must NOT count.
    """
    for var in KEY_VARS:
        val = os.environ.get(var)
        if val:
            return val
    env_file = (root if root is not None else REPO_ROOT) / ".env"
    try:
        lines = env_file.read_text().splitlines()
    except (FileNotFoundError, OSError):
        return None
    for line in lines:
        s = line.strip()
        if "=" not in s or s.startswith("#"):
            continue
        name, _, raw = s.partition("=")
        if name.strip() in KEY_VARS:
            val = raw.strip().strip("\"'")
            if val:
                return val
    return None


#: Skip marker for perceptual tests when no key is configured.
requires_gemini = pytest.mark.skipif(
    gemini_key() is None,
    reason="no Gemini API key (set GEMINI_API_KEY or GOOGLE_STUDIO_API_KEY, "
           "in the environment or the repo .env)",
)


# ── transport (module-level so tests can monkeypatch it; no HTTP in guards) ──

def _post(model: str, payload: dict, key: str, timeout: float) -> dict:
    """POST one generateContent request; return the decoded JSON body."""
    url = f"{_API_BASE}/{model}:generateContent"
    req = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={"content-type": "application/json", "x-goog-api-key": key},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:  # includes 404 for a wrong model — NO fallback
        body = ""
        try:
            body = e.read().decode("utf-8", "replace")[:500]
        except Exception:
            pass
        raise VisionCriticError(
            f"Gemini HTTP {e.code} for model '{model}': {body}"
        ) from e
    except urllib.error.URLError as e:
        raise VisionCriticError(f"Gemini request failed: {e.reason}") from e
    except TimeoutError as e:
        raise VisionCriticError(f"Gemini request timed out after {timeout}s") from e


# ── request plumbing ─────────────────────────────────────────────────────────

def _image_part(image_path: str | Path) -> dict:
    data = Path(image_path).read_bytes()
    return {"inline_data": {"mime_type": "image/png",
                            "data": base64.b64encode(data).decode("ascii")}}


def _extract_text(response: dict) -> str:
    try:
        parts = response["candidates"][0]["content"]["parts"]
    except (KeyError, IndexError, TypeError):
        raise VisionCriticError(
            "Gemini response had no candidates[0].content.parts: "
            + json.dumps(response)[:500]
        ) from None
    text = "".join(p.get("text", "") for p in parts if isinstance(p, dict))
    if not text.strip():
        raise VisionCriticError("Gemini response contained no text parts")
    return text


def _parse_json_reply(text: str) -> dict:
    """Parse the model's JSON reply; tolerate a stray markdown fence or a short
    prose preamble ("Here is the JSON: ..."), both observed live from
    gemini-3.5-flash even with responseMimeType=application/json (TASK-9)."""
    s = text.strip()
    if s.startswith("```"):
        s = s.strip("`")
        if s.lower().startswith("json"):
            s = s[4:]
        s = s.strip()
    if not s.startswith("{"):
        # Fall back to the first balanced {...} block anywhere in the reply.
        start = s.find("{")
        if start != -1:
            depth = 0
            for i in range(start, len(s)):
                if s[i] == "{":
                    depth += 1
                elif s[i] == "}":
                    depth -= 1
                    if depth == 0:
                        s = s[start:i + 1]
                        break
    try:
        obj = json.loads(s)
    except json.JSONDecodeError as e:
        raise VisionCriticError(f"critic reply is not valid JSON: {text[:300]}") from e
    if not isinstance(obj, dict):
        raise VisionCriticError(f"critic reply is not a JSON object: {text[:300]}")
    return obj


def _generate(image_path: str | Path, prompt: str, *, model: str,
              max_output_tokens: int, timeout: float) -> dict:
    key = gemini_key()
    if key is None:
        raise MissingKeyError(
            "No Gemini API key configured. Set GEMINI_API_KEY or "
            "GOOGLE_STUDIO_API_KEY (environment or repo .env)."
        )
    payload = {
        "contents": [{"parts": [{"text": prompt}, _image_part(image_path)]}],
        "generationConfig": {
            "temperature": 0,
            "responseMimeType": "application/json",
            "maxOutputTokens": max_output_tokens,
        },
    }
    return _parse_json_reply(_extract_text(_post(model, payload, key, timeout)))


# ── public API ───────────────────────────────────────────────────────────────

def ask(image_path: str | Path, question: str, *, model: str = DEFAULT_MODEL,
        votes: int = 1, timeout: float = _DEFAULT_TIMEOUT_S) -> bool:
    """Ask one narrow yes/no question about an image; return the verdict.

    ``votes > 1`` re-asks (one API call each) and returns the strict majority
    — use an odd count, only for questions that proved flaky. Raises
    :class:`MissingKeyError` with no key, :class:`VisionCriticError` on any
    transport/format failure.
    """
    if votes < 1:
        raise ValueError("votes must be >= 1")
    prompt = (
        "Look at the image and answer the yes/no question about it.\n"
        f"QUESTION: {question}\n"
        'Respond with ONLY a JSON object, no prose, no markdown, exactly of the '
        'form: {"answer": true, "confidence": 0.95} — where "answer" is the '
        "boolean verdict and \"confidence\" is your confidence in [0,1]."
    )
    yes = 0
    for _ in range(votes):
        reply = _generate(image_path, prompt, model=model,
                          max_output_tokens=2048, timeout=timeout)
        answer = reply.get("answer")
        if isinstance(answer, str):
            answer = {"true": True, "false": False}.get(answer.strip().lower())
        if not isinstance(answer, bool):
            raise VisionCriticError(
                f'critic reply lacks a boolean "answer": {reply!r}')
        yes += 1 if answer else 0
    return yes * 2 > votes


def ask_choice(image_path: str | Path, question: str, choices: Sequence[str], *,
               model: str = DEFAULT_MODEL,
               timeout: float = _DEFAULT_TIMEOUT_S) -> str:
    """Ask a closed multiple-choice question (e.g. CENTERED/EDGE/ABSENT).

    Returns the chosen option exactly as spelled in ``choices`` (the model's
    casing is normalized back). An answer outside ``choices`` raises
    :class:`VisionCriticError` — the test then fails loudly, never guesses.
    """
    if len(choices) < 2:
        raise ValueError("ask_choice needs at least 2 choices")
    menu = ", ".join(f'"{c}"' for c in choices)
    prompt = (
        "Look at the image and answer the question by picking EXACTLY ONE of "
        f"the allowed choices.\nQUESTION: {question}\nALLOWED CHOICES: {menu}\n"
        'Respond with ONLY a JSON object, no prose, no markdown, exactly of '
        'the form: {"choice": "<one of the allowed choices>"}.'
    )
    reply = _generate(image_path, prompt, model=model,
                      max_output_tokens=1024, timeout=timeout)
    choice = reply.get("choice")
    if not isinstance(choice, str):
        raise VisionCriticError(f'critic reply lacks a string "choice": {reply!r}')
    for c in choices:
        if choice.strip().lower() == c.lower():
            return c
    raise VisionCriticError(
        f"critic chose {choice!r}, not one of the allowed choices {list(choices)}")
