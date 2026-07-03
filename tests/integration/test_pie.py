"""Play-In-Editor (PIE) and AI-runtime domain. Driven through the real MCP
server (the `mcp` fixture calls tools by name).

Two classes of test live here:

* Headless-safe queries — ops that only *read* PIE state (or that fail cleanly
  with a documented `not_in_pie` guard when no session is running). These never
  open a viewport, so they run in both headless (-nullrhi) and gui modes. They
  assert the documented "no session" envelope.

* PIE lifecycle + input — ops that actually START PIE (and the input injectors
  that are only meaningful against a live PIE viewport). Starting PIE opens a
  game window, which can fatal the editor under -nullrhi via the Slate
  layout-save path, so every test that calls `start_pie` is `@pytest.mark.gui_only`
  (skipped unless --ue-mode=gui). After every risky op we `assert_ready`.

The AI-runtime ops (get_ai_*) require a live PIE world holding a perception-
enabled AIController — content the empty fixture project lacks. We exercise them
via their documented `not_in_pie` guard (headless-safe), which still dispatches
the op and counts toward coverage.

NOTE — the `send_key_input` operation is dispatched through the MCP
`send_keystrokes` tool (which takes an `actions` array and forwards each event
to the C++ `send_key_input` command).

Pattern for every test: arrange prerequisite state -> dispatch the op -> assert
the resulting state (or the documented guard envelope).
"""

import os
import struct
import time

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready


def _wait_for_pie(bridge, want_running: bool, timeout: float = 30.0) -> bool:
    """Poll get_pie_state until is_running matches `want_running` (PIE start/stop
    is asynchronous — the handlers return 'starting'/'stopping' immediately and
    the world flips state a few frames later). Returns the final is_running."""
    deadline = time.monotonic() + timeout
    state = {}
    while time.monotonic() < deadline:
        state = bridge.expect("pie_get_state", {})
        if bool(state.get("is_running")) == want_running:
            return want_running
        time.sleep(0.5)
    return bool(state.get("is_running"))


def _acquire_pie(mcp, requester: str, timeout: float = 120.0) -> dict:
    """Acquire the shared PIE lease, honoring the queue protocol: pie_busy is a
    queue position, not a failure — re-call pie_start to hold the place (FIFO)
    until promoted or the deadline passes (docs/USAGE.md §2.18)."""
    deadline = time.monotonic() + timeout
    while True:
        resp = mcp.command("pie_start", {"requester": requester})
        if resp.get("status") == "success":
            return resp
        if resp.get("error_code") != "pie_busy" or time.monotonic() >= deadline:
            pytest.fail(f"could not acquire the PIE lease: {resp}")
        time.sleep(2.0)


def _png_dimensions(path: str) -> tuple[int, int]:
    """Parse width/height straight out of the PNG IHDR chunk (an observation
    independent of the capture op's own envelope)."""
    with open(path, "rb") as fh:
        header = fh.read(24)
    assert header[:8] == b"\x89PNG\r\n\x1a\n", f"not a PNG: {header[:8]!r}"
    assert header[12:16] == b"IHDR", header
    width, height = struct.unpack(">II", header[16:24])
    return width, height


# ---------------------------------------------------------------------------
# Headless-safe: PIE-state queries without a running session
# ---------------------------------------------------------------------------

@covers("pie_get_state")
def test_get_pie_state_idle_reports_not_running(mcp):
    # get_pie_state is a success envelope even with no session — is_running=false.
    result = mcp.expect("pie_get_state", {})
    assert result.get("is_running") is False, result


@covers("pie_query")
def test_pie_query_without_pie_errors_not_in_pie(mcp):
    # pie_query reads the LIVE PIE world; with no session it returns the
    # documented not_in_pie guard (error envelope), not a success.
    resp = mcp.command("pie_query", {"query": "summary", "limit": 200})
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "not_in_pie", resp
    assert_ready(mcp)


@covers("pie_stop")
def test_stop_pie_without_session_errors_not_in_pie(mcp):
    # Stopping when nothing is playing is the not_in_pie guard. Safe headless:
    # no window is opened or torn down.
    resp = mcp.command("pie_stop", {})
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "not_in_pie", resp
    assert_ready(mcp)


# ---------------------------------------------------------------------------
# Headless-safe: AI-runtime ops via their not_in_pie guard
# ---------------------------------------------------------------------------

@covers("ai_get_state", "ai_get_awareness", "ai_get_perception")
def test_ai_runtime_ops_require_pie(mcp):
    # All three AI-runtime ops query the live PIE world for an AIController. With
    # no PIE session (the headless default) each returns the documented not_in_pie
    # guard. This dispatches every op (error path) without needing a window.
    for op in ("ai_get_state", "ai_get_awareness", "ai_get_perception"):
        resp = mcp.command(op, {"actor_name": "MCPTest_AIPawn"})
        if resp.get("status") == "success":
            # Only reachable inside an active PIE session with a real AI pawn.
            assert isinstance(resp.get("result"), dict), resp
            continue
        code = resp.get("error_code")
        if code != "not_in_pie":
            pytest.skip(
                f"{op} needs a live PIE session with a perception-enabled "
                f"AIController; got error_code={code!r}"
            )
        assert code == "not_in_pie", resp
    assert_ready(mcp)


# ---------------------------------------------------------------------------
# GUI-only: PIE lifecycle + input injection (opens a game viewport)
# ---------------------------------------------------------------------------

@pytest.mark.gui_only
@covers("pie_start", "pie_get_state", "pie_query",
        "pie_send_keystrokes", "pie_send_mouse", "pie_stop")
def test_pie_lifecycle_start_query_input_stop(mcp):
    # Make sure we begin from a clean (not-playing) state.
    if mcp.expect("pie_get_state", {}).get("is_running"):
        mcp.command("pie_stop", {})
        _wait_for_pie(mcp, want_running=False)

    # start_pie returns immediately ("starting"); the world goes live a few
    # frames later. It plays the currently-open level (no map_path needed).
    start = mcp.expect("pie_start", {})
    assert start.get("status") == "starting", start
    session_id = start.get("session_id")
    assert session_id, start
    assert_ready(mcp)

    assert _wait_for_pie(mcp, want_running=True), "PIE never reached is_running"

    # get_pie_state now reports the live session id back.
    state = mcp.expect("pie_get_state", {})
    assert state.get("is_running") is True, state
    assert state.get("session_id") == session_id, state

    # pie_query against the live world now succeeds (summary shape).
    summary = mcp.expect("pie_query", {"query": "summary", "limit": 200})
    assert isinstance(summary, dict) and summary, summary

    # Inject a key tap (pressed + released pair) and a mouse move. send_keystrokes
    # wraps the bridge send_key_input events; assert the batch reports success.
    down = mcp.expect("pie_send_keystrokes", {"actions": [{"key": "W", "event_type": "pressed"}]})
    assert down.get("success") is not False, down
    mcp.expect("pie_send_keystrokes", {"actions": [{"key": "W", "event_type": "released"}]})

    moved = mcp.expect("pie_send_mouse",
                       {"x": 100.0, "y": 100.0, "event_type": "move", "button": "left"})
    assert moved.get("event_type") == "move", moved
    assert_ready(mcp)

    # stop_pie validates the session id, then ends the session asynchronously.
    stop = mcp.expect("pie_stop", {"session_id": session_id})
    assert stop.get("status") == "stopping", stop
    assert _wait_for_pie(mcp, want_running=False), "PIE never stopped"
    assert_ready(mcp)


# ---------------------------------------------------------------------------
# Pose capture + Enhanced Input injection
# ---------------------------------------------------------------------------

@covers("pie_capture_from_pose")
def test_pie_capture_from_pose_without_pie_errors_not_in_pie(mcp):
    # The capture rig reproduces a pose INSIDE the running game; with no PIE
    # session it returns the documented not_in_pie guard (checked before any
    # camera is spawned or file written). Safe headless: nothing renders.
    resp = mcp.command("pie_capture_from_pose", {
        "location": {"x": 0.0, "y": 0.0, "z": 500.0},
        "rotation": {"pitch": -90.0, "yaw": 0.0, "roll": 0.0},
    })
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "not_in_pie", resp
    assert_ready(mcp)


@covers("pie_inject_input_action")
def test_pie_inject_input_action_without_pie_errors(mcp):
    # Enhanced Input injection targets the live PIE player. With no session the
    # handler refuses BEFORE loading the asset — the documented guard is
    # invalid_argument ("PIE is not running"), not not_in_pie.
    resp = mcp.command("pie_inject_input_action", {
        "action_path": "/Game/__MCPTest__/pie/IA_DoesNotExist",
    })
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "invalid_argument", resp
    assert "PIE" in str(resp.get("error", "")), resp
    assert_ready(mcp)


@pytest.mark.gui_only
@covers("pie_capture_from_pose", "pie_inject_input_action")
def test_pie_capture_from_pose_and_inject_input_action(mcp, tmp_path):
    # One short PIE session exercises both ops (keeps shared-editor PIE time
    # minimal). The pose IS the fixed capture rig — a supplied location/rotation/
    # fov, zero navigation — per docs/TESTING.md.
    #
    # Capture observation: the PNG on disk (signature + IHDR dimensions + size),
    # read back by the test itself — independent of the op's envelope.
    # Injection observation: the positive path asserts the injection ack only
    # (no typed primitive can bind an InputAction to an observable consumer
    # without C++ — the deep consumer-side observation is #DEFERRED in
    # docs/loops/tests/TASKS.md); the bad-path asserts the documented
    # asset_not_found guard, which only fires inside a live session.
    ns = "/Game/__MCPTest__/pie"
    ia_path = f"{ns}/IA_MCPInjectProbe"

    # Arrange (pre-PIE — asset mutation stays out of the session): a real
    # UInputAction under the test namespace for the positive injection.
    ensure_absent(mcp, ia_path)
    created = mcp.expect("input_create", {
        "type": "action", "name": "IA_MCPInjectProbe", "path": ns,
        "value_type": "boolean",
    })
    assert created.get("asset_path"), created

    acquired = False
    try:
        _acquire_pie(mcp, requester="pytest pie capture/inject")
        acquired = True
        assert _wait_for_pie(mcp, want_running=True), "PIE never reached is_running"

        # Act: capture from an explicit top-down pose into a test-owned dir.
        result = mcp.expect("pie_capture_from_pose", {
            "location": {"x": 0.0, "y": 0.0, "z": 500.0},
            "rotation": {"pitch": -90.0, "yaw": 0.0, "roll": 0.0},
            "fov": 90.0,
            "filename": "pytest_pose_capture",
            "directory": str(tmp_path),
        })
        # The bridge confirms the file before returning: status flips
        # "requested" -> "captured" only once the PNG exists on disk.
        assert result.get("status") == "captured", result
        path = result.get("path") or result.get("file_path")
        assert path, result

        # Observe: the file itself — a real, non-trivial PNG with sane dims.
        assert os.path.isfile(path), f"capture not written to {path}"
        assert os.path.getsize(path) > 1000, os.path.getsize(path)
        width, height = _png_dimensions(path)
        assert width > 0 and height > 0, (width, height)
        assert_ready(mcp)

        # Injecting a nonexistent action inside the live session is the
        # documented asset_not_found guard (the PIE gate passed first).
        bad = mcp.command("pie_inject_input_action", {
            "action_path": f"{ns}/IA_DoesNotExist",
        })
        assert bad.get("status") == "error", bad
        assert bad.get("error_code") == "asset_not_found", bad

        # Positive injection into the live player's EnhancedInput subsystem.
        injected = mcp.expect("pie_inject_input_action", {
            "action_path": ia_path, "value": 1.0,
        })
        assert injected.get("injected") is True, injected
        assert injected.get("action") == ia_path, injected
        assert_ready(mcp)
    finally:
        if acquired:
            mcp.command("pie_stop", {})
            _wait_for_pie(mcp, want_running=False)
        mcp.command("asset_delete", {"asset_path": ia_path, "force": True})
        assert_ready(mcp)


# ---------------------------------------------------------------------------
# PIE video recording (pie_record_*)
# ---------------------------------------------------------------------------

@covers("pie_record_status")
def test_pie_record_status_idle_reports_not_recording(mcp):
    # Read-only; safe in every mode. With no recording it reports recording=false.
    result = mcp.expect("pie_record_status", {})
    assert result.get("recording") is False, result


@covers("pie_record_arm", "pie_record_disarm")
def test_pie_record_arm_disarm_roundtrip(mcp):
    # Arming outside PIE is valid (it latches onto the NEXT session); disarm
    # reports the armed flag flipping back off. Safe in every mode.
    armed = mcp.expect("pie_record_arm", {"base_name": "pytest_take"})
    assert armed.get("armed") is True, armed
    status = mcp.expect("pie_record_status", {})
    assert status.get("armed") is True, status
    disarmed = mcp.expect("pie_record_disarm", {})
    assert disarmed.get("armed") is False, disarmed
    assert_ready(mcp)


@covers("pie_record_start", "pie_record_stop")
def test_pie_record_guards_without_session(mcp):
    # Stop with nothing active is the documented invalid_argument guard.
    resp = mcp.command("pie_record_stop", {})
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") == "invalid_argument", resp

    # Start outside PIE is refused: not_in_pie in gui mode; under -nullrhi the
    # RHI guard fires first (feature_disabled — no frames exist to record).
    resp = mcp.command("pie_record_start", {"fps": 30, "max_duration_s": 5})
    assert resp.get("status") == "error", resp
    assert resp.get("error_code") in ("not_in_pie", "feature_disabled"), resp
    assert_ready(mcp)


@pytest.mark.gui_only
@covers("pie_record_start", "pie_record_status", "pie_record_stop")
def test_pie_record_lifecycle_produces_mp4(mcp, tmp_path):
    # Arrange: a live PIE session (real RHI — gui_only).
    if mcp.expect("pie_get_state", {}).get("is_running"):
        mcp.command("pie_stop", {})
        _wait_for_pie(mcp, want_running=False)
    start = mcp.expect("pie_start", {})
    assert start.get("status") == "starting", start
    assert _wait_for_pie(mcp, want_running=True), "PIE never reached is_running"

    try:
        # Act: record ~3 seconds of the live viewport.
        rec = mcp.expect("pie_record_start",
                         {"fps": 30, "max_duration_s": 30, "filename": "pytest_record"})
        assert rec.get("recording_id"), rec
        path = rec.get("path")
        assert path and path.endswith(".mp4"), rec

        time.sleep(3.0)
        status = mcp.expect("pie_record_status", {})
        assert status.get("recording") is True, status
        assert status.get("frames_captured", 0) > 0, status

        done = mcp.expect("pie_record_stop", {})
        # Assert: a real, non-trivial MP4 landed where the envelope says.
        assert done.get("frames_encoded", 0) > 0, done
        assert done.get("bytes", 0) > 0, done
        import os
        assert os.path.isfile(path), f"missing recording at {path}"
        with open(path, "rb") as fh:
            header = fh.read(12)
        assert header[4:8] == b"ftyp", header  # valid MP4 container
    finally:
        mcp.command("pie_record_stop", {})  # idempotent-safe cleanup
        mcp.command("pie_stop", {})
        _wait_for_pie(mcp, want_running=False)
        assert_ready(mcp)
