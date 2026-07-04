"""Skill-test loop TASK-8 — guard tests for the vision-critic helper.

Validates ``tests/harness/vision_critic.py`` — the Gemini-backed perceptual
verifier that TASK-9/10/11 build on — WITHOUT an editor and WITHOUT the
network:

* key resolution (:func:`gemini_key`) mirrors
  ``.claude/hooks/gemini-cred-gate.py`` ``key_configured()``: env var first,
  else the repo ``.env``; both ``GEMINI_API_KEY`` and ``GOOGLE_STUDIO_API_KEY``
  spellings; exact-name match only (``GOOGLE_STUDIO_API_KEY_NAME`` must not
  count).
* with the key forcibly absent, :func:`ask` raises the documented
  :class:`MissingKeyError` and :func:`gemini_key` returns None.
* JSON parsing of canned generateContent payloads — transport is stubbed
  (monkeypatched ``_post``), no HTTP ever leaves the process.
* payload shape matches the critique.ts recipe: pinned model, temperature 0,
  ``responseMimeType: application/json``, image as inline_data base64 PNG.

Plus one OPTIONAL live round-trip smoke (``perceptual``-marked, skips without
a key): a locally generated solid-red PNG (stdlib zlib/struct — no PIL dep),
asked a red question (expect True) and a blue control question (expect False)
— the control arm is the anti-rubber-stamp differential. Exactly 2 API calls.

Headless-safe: no bridge/mcp fixtures are requested, so collection/run never
boots an editor.
"""

from __future__ import annotations

import json
import struct
import zlib
from pathlib import Path

import pytest

from harness import vision_critic as vc


# ── local fixtures ───────────────────────────────────────────────────────────

@pytest.fixture
def no_key(monkeypatch, tmp_path):
    """Forcibly remove every key source: env vars gone, .env lookup pointed at
    an empty temp dir (never the real repo root)."""
    for var in vc.KEY_VARS:
        monkeypatch.delenv(var, raising=False)
    monkeypatch.setattr(vc, "REPO_ROOT", tmp_path)
    return tmp_path


@pytest.fixture
def png(tmp_path) -> Path:
    p = tmp_path / "img.png"
    _write_solid_png(p, (255, 0, 0))
    return p


def _write_solid_png(path: Path, rgb: tuple[int, int, int], size: int = 64) -> None:
    """Minimal valid RGB PNG, stdlib only (no PIL dependency in the harness)."""
    raw = b"".join(b"\x00" + bytes(rgb) * size for _ in range(size))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
                     + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def _canned(text: str) -> dict:
    """A generateContent response body carrying `text` in the standard place."""
    return {"candidates": [{"content": {"parts": [{"text": text}]}}]}


def _stub_post(monkeypatch, replies: list[str]) -> list[dict]:
    """Replace the transport with a canned-reply queue; record each payload."""
    calls: list[dict] = []

    def fake_post(model, payload, key, timeout):
        calls.append({"model": model, "payload": payload, "key": key})
        return _canned(replies[min(len(calls) - 1, len(replies) - 1)])

    monkeypatch.setattr(vc, "_post", fake_post)
    return calls


# ── key resolution ───────────────────────────────────────────────────────────

def test_gemini_key_none_when_absent(no_key):
    assert vc.gemini_key() is None


def test_gemini_key_prefers_env(no_key, monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "env-key")
    assert vc.gemini_key() == "env-key"
    monkeypatch.delenv("GEMINI_API_KEY")
    monkeypatch.setenv("GOOGLE_STUDIO_API_KEY", "studio-key")
    assert vc.gemini_key() == "studio-key"


def test_gemini_key_from_dotenv(no_key):
    (no_key / ".env").write_text('GOOGLE_STUDIO_API_KEY="file-key"\n')
    assert vc.gemini_key() == "file-key"


def test_gemini_key_exact_name_only(no_key):
    # The hook's startswith() would wrongly match this; the helper must not.
    (no_key / ".env").write_text(
        "GOOGLE_STUDIO_API_KEY_NAME=not-a-key\n# GEMINI_API_KEY=commented\n")
    assert vc.gemini_key() is None


# ── no-key error path (no network possible: transport would need a key) ─────

def test_ask_without_key_raises_missing_key(no_key, png, monkeypatch):
    # Belt and suspenders: if key resolution somehow passed, fail loudly
    # instead of touching the network.
    monkeypatch.setattr(
        vc, "_post",
        lambda *a, **k: pytest.fail("transport called despite missing key"))
    with pytest.raises(vc.MissingKeyError):
        vc.ask(png, "Is the image red?")
    with pytest.raises(vc.MissingKeyError):
        vc.ask_choice(png, "Where is it?", ["CENTERED", "EDGE", "ABSENT"])


# ── JSON parsing + request shape against canned payloads (stubbed transport) ─

def test_ask_parses_true_and_request_shape(no_key, png, monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "k")
    calls = _stub_post(monkeypatch, ['{"answer": true, "confidence": 0.97}'])
    assert vc.ask(png, "Is the image red?") is True
    assert len(calls) == 1
    assert calls[0]["model"] == "gemini-3.5-flash"  # pinned, no fallback
    gc = calls[0]["payload"]["generationConfig"]
    assert gc["temperature"] == 0
    assert gc["responseMimeType"] == "application/json"
    parts = calls[0]["payload"]["contents"][0]["parts"]
    assert "Is the image red?" in parts[0]["text"]
    inline = parts[1]["inline_data"]
    assert inline["mime_type"] == "image/png"
    import base64
    assert base64.b64decode(inline["data"]).startswith(b"\x89PNG")


def test_ask_parses_false_and_fenced_json(no_key, png, monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "k")
    _stub_post(monkeypatch,
               ['```json\n{"answer": false, "confidence": 0.9}\n```'])
    assert vc.ask(png, "Is the image blue?") is False


def test_ask_majority_vote(no_key, png, monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "k")
    calls = _stub_post(monkeypatch, [
        '{"answer": true, "confidence": 0.8}',
        '{"answer": false, "confidence": 0.6}',
        '{"answer": true, "confidence": 0.9}',
    ])
    assert vc.ask(png, "flaky question", votes=3) is True
    assert len(calls) == 3


def test_ask_rejects_garbage_reply(no_key, png, monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "k")
    _stub_post(monkeypatch, ["the image sure looks red to me"])
    with pytest.raises(vc.VisionCriticError):
        vc.ask(png, "Is the image red?")


def test_ask_rejects_missing_answer_field(no_key, png, monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "k")
    _stub_post(monkeypatch, ['{"verdict": "yes"}'])
    with pytest.raises(vc.VisionCriticError):
        vc.ask(png, "Is the image red?")


def test_ask_rejects_empty_candidates(no_key, png, monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "k")
    monkeypatch.setattr(vc, "_post", lambda *a, **k: {"candidates": []})
    with pytest.raises(vc.VisionCriticError):
        vc.ask(png, "Is the image red?")


def test_ask_choice_normalizes_casing(no_key, png, monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "k")
    _stub_post(monkeypatch, ['{"choice": "centered"}'])
    got = vc.ask_choice(png, "Where is the cube?",
                        ["CENTERED", "EDGE", "ABSENT"])
    assert got == "CENTERED"


def test_ask_choice_rejects_off_menu_answer(no_key, png, monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "k")
    _stub_post(monkeypatch, ['{"choice": "SOMEWHERE"}'])
    with pytest.raises(vc.VisionCriticError):
        vc.ask_choice(png, "Where?", ["CENTERED", "EDGE", "ABSENT"])


def test_ask_votes_must_be_positive(no_key, png):
    with pytest.raises(ValueError):
        vc.ask(png, "q", votes=0)


# ── optional live round-trip smoke (2 API calls, skips without a key) ───────

@pytest.mark.perceptual
@vc.requires_gemini
def test_live_smoke_red_square_round_trip(tmp_path):
    """Prove the real round-trip once: a locally drawn solid-red PNG must read
    as red (positive arm) and NOT as blue (control arm — anti-rubber-stamp)."""
    p = tmp_path / "red.png"
    _write_solid_png(p, (220, 20, 20))
    assert vc.ask(p, "Is the dominant color of this image red?") is True
    assert vc.ask(p, "Is the dominant color of this image blue?") is False
