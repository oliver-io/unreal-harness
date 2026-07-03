"""Pytest wiring for the UnrealMCP integration suite.

Fixtures
--------
``editor_session`` (session-scoped)
    Boots ONE editor for the whole run (boot is expensive — tens of seconds to
    minutes) and tears it down at the end. Mode is chosen by ``--ue-mode``.

``bridge`` (function-scoped)
    A ready ``BridgeClient`` bound to that editor. This is what tests use.

``mcp_namespace`` (function-scoped)
    A ``/Game/__MCPTest__/<test>`` content path for tests that must save assets,
    with best-effort teardown. Actor-spawn tests don't need it — spawned actors
    live in the unsaved transient level and vanish when the editor quits.

Options
-------
``--ue-mode=headless|gui``   default headless. ``gui`` enables render tests.
``--ue-build=auto|always|never``   default auto (build iff not already built).
``--ue-attach``   don't launch/build; assume an editor is already listening
                  (fast local iteration against your own running editor).
"""

from __future__ import annotations

import os

import pytest

from harness import config
from harness.bridge_client import BridgeClient
from harness.editor import EditorSession, reset_test_content
from harness.mcp_client import MCPClient
from harness.server import MCPServer


def pytest_addoption(parser):
    g = parser.getgroup("unreal")
    g.addoption("--ue-mode", action="store", default="headless",
                choices=("headless", "gui"),
                help="headless (-nullrhi, no render) or gui (real window, enables render tests)")
    g.addoption("--ue-build", action="store", default="auto",
                choices=("auto", "always", "never"),
                help="build the editor target before launch")
    g.addoption("--ue-attach", action="store_true", default=False,
                help="attach to an already-running editor instead of launching one")


# NOTE: pytest matches hook args by name, so the parameter MUST be `config`
# (it locally shadows the harness `config` module, which these hooks don't use).
def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "render: test needs real rendering (screenshots/thumbnails). "
        "Skipped unless --ue-mode=gui.",
    )
    config.addinivalue_line(
        "markers",
        "gui_only: test opens editor windows / asset editors. Unsafe under -nullrhi "
        "(the periodic Slate layout-save ticker fatals on a null window), so it is "
        "skipped unless --ue-mode=gui.",
    )


@pytest.fixture(scope="session")
def ue_mode(request) -> str:
    return request.config.getoption("--ue-mode")


def _assert_attached_editor_is_test_project(bridge: BridgeClient) -> None:
    """--ue-attach trusts an already-running editor — but ONLY one hosting the
    test project (UE_MCP_TEST_PROJECT, default the fixture). Attaching blindly
    is how test mutations end up inside a real project's Content. Identity comes
    from the editor's own project_context (settings_paths[0] ==
    FPaths::ProjectDir()). Escape hatch: UE_MCP_ATTACH_ANY=1."""
    if os.environ.get("UE_MCP_ATTACH_ANY") == "1":
        return
    ctx = bridge.expect("project_context", {})
    hosted = str((ctx.get("settings_paths") or [""])[0])
    hosted_n = os.path.normcase(os.path.normpath(os.path.abspath(hosted)))
    expected_n = os.path.normcase(os.path.normpath(str(config.project_dir())))
    if hosted_n != expected_n:
        pytest.fail(
            f"--ue-attach: the editor on {config.BRIDGE_HOST}:{config.BRIDGE_PORT} hosts "
            f"'{hosted}', not the test project '{config.project_dir()}'. Refusing to run "
            "tests against a project the harness doesn't own. Point UE_MCP_TEST_PROJECT "
            "at that project to target it deliberately, or set UE_MCP_ATTACH_ANY=1 to "
            "bypass this guard.",
            pytrace=False,
        )


@pytest.fixture(scope="session")
def editor_session(request, ue_mode):
    """Boot (or attach to) the editor once per session."""
    if request.config.getoption("--ue-attach"):
        bridge = BridgeClient(config.BRIDGE_HOST, config.BRIDGE_PORT)
        if not bridge.wait_ready(timeout=30):
            pytest.fail(
                "--ue-attach set but no interactive editor is listening on "
                f"{config.BRIDGE_HOST}:{config.BRIDGE_PORT}"
            )
        _assert_attached_editor_is_test_project(bridge)
        yield None  # nothing to own/tear down
        return

    session = EditorSession(
        mode=ue_mode,
        build=request.config.getoption("--ue-build"),
    )
    try:
        session.start()
    except Exception as e:  # surface boot failures as a clean session error
        pytest.fail(f"failed to start editor: {e}", pytrace=False)
    yield session
    session.stop()


@pytest.fixture
def bridge(editor_session) -> BridgeClient:
    """Function-scoped client. Re-verifies readiness so a test never runs against
    a half-dead editor. If the session owns the editor and it has died (some ops
    fatal the -nullrhi process — see the gui_only marker), transparently restart
    it so one crash doesn't cascade into every later test."""
    if editor_session is None:  # --ue-attach: we don't own the process
        client = BridgeClient(config.BRIDGE_HOST, config.BRIDGE_PORT)
        if not client.is_ready():
            pytest.fail(
                "attached editor is not interactive (it may have crashed; "
                "restart it). mcp_status.ready is false."
            )
        return client

    if not editor_session.alive_and_ready():
        editor_session.restart()
    return editor_session.bridge


# ── MCP-path fixtures (the REAL product surface) ────────────────────────────
# Tests use the `mcp` fixture, which drives the Python MCP server over its
# streamable-HTTP endpoint — calling tools by name with their tool kwargs,
# exercising the kwarg→bridge-param mapping / orchestration / response shaping
# that the raw `bridge` fixture skips. The `bridge` fixture remains for low-level
# isolation/cleanup helpers.

@pytest.fixture(scope="session")
def mcp_server(editor_session):
    """Launch the Python MCP server once (after the editor is up). When we own
    the editor, own a FRESH server too: a listener already on 8765 is a stale
    leak whose tool catalog won't match the code under test. In attach mode
    (editor_session is None) deliberately reuse the running dev server."""
    srv = MCPServer()
    try:
        srv.start(fresh=editor_session is not None)
    except Exception as e:
        pytest.fail(f"failed to start MCP server: {e}", pytrace=False)
    yield srv
    srv.stop()


@pytest.fixture(scope="session")
def _mcp_client(mcp_server):
    client = MCPClient().connect()
    if not client.wait_ready_via_status(timeout=60):
        client.close()
        pytest.fail("MCP server up but editor never reported ready via mcp_status")
    yield client
    client.close()


@pytest.fixture
def mcp(_mcp_client, editor_session):
    """Function-scoped MCP client. Re-verifies readiness and restarts a dead
    editor (the server reconnects to the bridge automatically) so one crash
    doesn't cascade."""
    if editor_session is not None and not editor_session.alive_and_ready():
        editor_session.restart()
    if not _mcp_client.is_ready():
        pytest.fail("editor is not interactive via the MCP server (mcp_status not ready)")
    return _mcp_client


@pytest.fixture
def mcp_namespace(bridge, request):
    """Yield a unique /Game/__MCPTest__/<test> path; clean it up afterward."""
    safe = "".join(c if (c.isalnum() or c == "_") else "_" for c in request.node.name)
    path = f"/Game/{config.TEST_CONTENT_NAMESPACE}/{safe}"
    yield path
    # Best-effort in-editor delete of the folder, then nuke the on-disk dir at
    # session end (reset_test_content also runs in the session finalizer below).
    try:
        bridge.command("asset_delete", {"asset_path": path, "force": True})
    except Exception:
        pass


@pytest.fixture(scope="session", autouse=True)
def _final_disk_reset():
    """Belt-and-suspenders: wipe Content/__MCPTest__ on disk after the run, so a
    test that saved assets never leaves the fixture project dirty."""
    yield
    reset_test_content()


def pytest_collection_modifyitems(config, items):
    """Skip render/gui_only-marked tests unless we're in gui mode."""
    if config.getoption("--ue-mode") == "gui":
        return
    skip_render = pytest.mark.skip(reason="needs --ue-mode=gui (real rendering)")
    skip_gui = pytest.mark.skip(reason="needs --ue-mode=gui (opens editor windows; "
                                "unsafe under -nullrhi)")
    for item in items:
        if "render" in item.keywords:
            item.add_marker(skip_render)
        if "gui_only" in item.keywords:
            item.add_marker(skip_gui)
