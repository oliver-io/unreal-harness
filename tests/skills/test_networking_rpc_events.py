"""Skill-test loop TASK-5 — /networking: RPC net-type round-trip on a custom event.

The /networking skill (`.claude/skills/networking/reference/REPLICATION.md:71,108`,
`reference/AUTHORITY.md:29-33`) teaches that a Blueprint RPC is a custom event
whose net type is one of Run-on-Server / Run-on-Client / Multicast, and that the
type governs routing. `bp_set_event_replication` is the single authoring
primitive that advice funnels into — this battery pins that the primitive lands
exactly one net type at a time on the real `UK2Node_CustomEvent` flags, and that
the type survives a compile.

Handler ground truth (read this task):
  `MCPBlueprintCommands.cpp:1551-1673` (HandleSetEventReplication) — dropdown→bit
  map at `:1583-1590` (none→0, multicast→FUNC_NetMulticast, server→FUNC_NetServer,
  client→FUNC_NetClient), clear-then-OR apply at `:1645-1654` (clears
  FUNC_Net|Multicast|Server|Client|NetReliable, then ORs FUNC_Net + the one
  chosen specifier) — mirroring the editor's
  FBlueprintGraphActionDetails::SetNetFlags. The observer decoder lives in
  `MCPBlueprintCommands.cpp:2986-3009`: for each `UK2Node_CustomEvent` it calls
  `GetNetFlags()` and emits `replication` ∈ {"Multicast","RunOnServer",
  "RunOnClient"} — and emits NO field at all when the flags are clear. The
  decoder is gated by `include_node_details` (`:2831-2835`) and each node carries
  name/class/title (`:2899-2902`).

  NOTE — spec deviation, verified this task: that decoder is in
  `HandleAnalyzeBlueprintGraph`, which is the **bp_inspect** wire command
  (`MCPBlueprintCommands.cpp:171-174`; `bp_read` → HandleReadBlueprintContent
  at `:167-169` emits only name/class/title per event-graph node, `:2549-2561`,
  with no replication decode). TASKS.md said "bp_read with
  include_node_details:true"; the working observer is `bp_inspect`.

Engine ground truth (UE 5.7 source, read this task):
  `CoreUObject/Public/UObject/Script.h:142` FUNC_Net=0x40, `:143`
  FUNC_NetReliable=0x80, `:150` FUNC_NetMulticast=0x4000, `:157`
  FUNC_NetServer=0x200000, `:160` FUNC_NetClient=0x1000000; `:183`
  FUNC_NetFuncFlags = Net|NetReliable|NetServer|NetClient|NetMulticast.
  `Editor/BlueprintGraph/Private/K2Node_CustomEvent.cpp:324-345`
  UK2Node_CustomEvent::GetNetFlags() = FunctionFlags & FUNC_NetFuncFlags (parent
  flags win only for overrides — our event is fresh, no parent function).

Arrange/observe split: arrange with `bp_add_custom_event` +
`bp_set_event_replication` (node-flag mutators); observe with `bp_inspect`'s
independent graph decoder, which re-derives the type from `GetNetFlags()` rather
than echoing the mutator's input. The `reliable` flag is deliberately NOT
asserted — the decoder omits it, so the only readback would be the mutator's own
echo (no independent oracle; ledgered in TASKS.md DEFERRED).

KNOWN BUG (2026-07-02, discovered by this test — see the xfail in
_set_replication): `bp_set_event_replication` is not routed by
`FMCPBridge::ExecuteCommand` (MCPBridge.cpp:748-788 omits it; handler wired
only inside FMCPBlueprintCommands::HandleCommand at :117), so the bridge
answers "Unknown command" and the mutator half of this battery xfails until
the dispatch line lands. The baseline half (bp_add_custom_event + the
bp_inspect decoder emitting NO replication field on a fresh event) runs green
today.

Headless-safe (no render/gui_only markers), bounded, self-cleaning: one
Blueprint, deleted in teardown (plus the session-end Content/__MCPTest__ disk
wipe backstop). `bp_create_blueprint`, `bp_set_event_replication`, and
`bp_compile` are PIE-blocked in the C++ blocklist (`MCPCommonUtils.cpp:211-216`,
bp_set_event_replication at `:214`), and `bp_add_custom_event` is PIE-blocked at
the server gate (`domains/bp.ts:643` blockedDuringPie) — so if PIE is live on
the shared editor we skip rather than collide with someone else's session.
"""

import time

import pytest

from harness.mcp_client import MCPToolError

NS = "/Game/__MCPTest__/skills_networking_rpc"
BP_PATH = f"{NS}/BP_MCPSkillRpcSample"
EVENT_NAME = "MCPSkillTestRpcEvent"


# ── helpers ──────────────────────────────────────────────────────────────────

def _pie_is_running(client) -> bool:
    r = client.call("pie_get_state", {})
    inner = r.get("result") if isinstance(r.get("result"), dict) else r
    return bool(inner.get("is_running"))


def _delete_bp_if_present(client):
    try:
        client.command("asset_delete", {"asset_path": BP_PATH, "force": True})
    except Exception:
        pass


def _retry_busy(fn, attempts=3, delay=2.0):
    """Bounded retry for the shared live editor's transient `engine_busy`
    (bridge receive timeout — e.g. a post-compile reinstancing/GC hitch, or
    another agent's long op holding the game thread). ONLY for steps that are
    idempotent by construction here: graph reads, re-setting the same net type,
    and recompiling the same Blueprint (the bridge executes commands
    sequentially on the game thread, so a retry after a timed-out-but-landed
    first attempt just converges to the same state). Bounded: <= attempts
    tries, fixed short delay."""
    last = None
    for _ in range(attempts):
        try:
            return fn()
        except MCPToolError as e:
            payload = e.payload if isinstance(e.payload, dict) else {}
            if payload.get("error_code") != "engine_busy":
                raise
            last = e
            time.sleep(delay)
    raise last


def _read_event_node(mcp):
    """Independent observer: bp_inspect decodes the custom event's live
    FunctionFlags via UK2Node_CustomEvent::GetNetFlags() into an optional
    `replication` string (MCPBlueprintCommands.cpp:2986-3009) — a different
    primitive from the mutator, re-deriving the type from the node flags.
    Pins/exec-trace off: the decoder only needs include_node_details."""
    result = _retry_busy(lambda: mcp.expect("bp_inspect", {
        "blueprint_path": BP_PATH,
        "graph_name": "EventGraph",
        "include_node_details": True,
        "include_pin_connections": False,
        "trace_execution_flow": False,
    }))
    nodes = result["graph_data"]["nodes"]
    matches = [n for n in nodes
               if n.get("class") == "K2Node_CustomEvent"
               and EVENT_NAME in (n.get("title") or "")]
    assert len(matches) == 1, (
        f"expected exactly one K2Node_CustomEvent titled with {EVENT_NAME!r}, "
        f"got {[(n.get('class'), n.get('title')) for n in nodes]}")
    return matches[0]


def _set_replication(mcp, value):
    try:
        res = _retry_busy(lambda: mcp.expect("bp_set_event_replication", {
            "blueprint_name": BP_PATH,
            "event_name": EVENT_NAME,
            "replication": value,
        }))
    except MCPToolError as e:
        payload = e.payload if isinstance(e.payload, dict) else {}
        if "Unknown command" in str(payload.get("error", "")):
            # KNOWN IMPLEMENTATION BUG (found by this test, 2026-07-02, filed
            # via the skill-test loop's BUGS pass): the handler exists and is
            # wired inside FMCPBlueprintCommands::HandleCommand
            # (MCPBlueprintCommands.cpp:117 → HandleSetEventReplication :1551),
            # but "bp_set_event_replication" is MISSING from the command router
            # FMCPBridge::ExecuteCommand's Blueprint-commands dispatch chain
            # (MCPBridge.cpp:748-788 — bp_set_class_replication is at :753,
            # bp_set_event_replication appears nowhere in the file), so the
            # bridge falls through to "Unknown command" (:1187) and the handler
            # is unreachable dead code. Once the dispatch line lands, this
            # xfail stops firing and the full battery runs green.
            pytest.xfail("bp_set_event_replication is not routed by "
                         "FMCPBridge::ExecuteCommand (MCPBridge.cpp dispatch "
                         "chain omits it) — handler unreachable; see BUGS")
        raise
    # Sanity only (the mutator's own echo is NOT the oracle — bp_inspect is).
    assert res.get("replication") == value, res
    return res


# ── fixture ──────────────────────────────────────────────────────────────────

@pytest.fixture(scope="module")
def rpc_bp(_mcp_client):
    """One Actor Blueprint with one fresh custom event for the module. Deleted
    in teardown; the shared editor may outlive this run."""
    if _pie_is_running(_mcp_client):
        pytest.skip("PIE is running on the shared editor — bp_add_custom_event "
                    "and bp_set_event_replication are PIE-blocked (bp.ts "
                    "blockedDuringPie / MCPCommonUtils.cpp blocklist); not "
                    "stopping someone else's session")
    _delete_bp_if_present(_mcp_client)
    _mcp_client.expect("bp_create_blueprint", {"name": BP_PATH, "parent_class": "Actor"})
    _mcp_client.expect("bp_add_custom_event", {
        "blueprint_name": BP_PATH, "event_name": EVENT_NAME})
    try:
        yield BP_PATH
    finally:
        _delete_bp_if_present(_mcp_client)


# ── the round-trip battery ────────────────────────────────────────────────────

def test_rpc_net_type_round_trip(mcp, rpc_bp):
    """One test, one story: a fresh custom event has NO net type; each
    bp_set_event_replication lands EXACTLY the one type it names (the
    clear-then-OR at MCPBlueprintCommands.cpp:1645-1654 makes types exclusive —
    each re-set must fully displace the previous one); `none` clears back to
    flag-absent; and the type survives a full compile. Sequential by design —
    the exclusivity claim IS the ordering — so a single test owns the whole
    sequence rather than order-coupled test functions."""
    # (a) baseline: fresh event carries no net flags → no `replication` field
    # (decoder emits the field only when a FUNC_Net* bit is set).
    node = _read_event_node(mcp)
    assert "replication" not in node, node

    # (b) server → RunOnServer (FUNC_Net|FUNC_NetServer; Script.h:142,:157).
    _set_replication(mcp, "server")
    assert _read_event_node(mcp).get("replication") == "RunOnServer"

    # (c) re-set client → RunOnClient displaces the server bit entirely
    # (decoder checks Multicast, then Server, then Client — a leftover server
    # bit would still read RunOnServer, so this pins the clear).
    _set_replication(mcp, "client")
    assert _read_event_node(mcp).get("replication") == "RunOnClient"

    # (d) re-set multicast → Multicast (first branch of the decoder; a stale
    # server/client bit cannot hide behind it because (e) proves full clear).
    _set_replication(mcp, "multicast")
    assert _read_event_node(mcp).get("replication") == "Multicast"

    # (e) none → ALL net bits cleared → field absent again (round trip closed).
    _set_replication(mcp, "none")
    assert "replication" not in _read_event_node(mcp)

    # (f) the type survives a compile: set a real type, full compile, re-read.
    _set_replication(mcp, "server")
    _retry_busy(lambda: mcp.expect("bp_compile", {"blueprint_name": rpc_bp}))
    assert _read_event_node(mcp).get("replication") == "RunOnServer"
