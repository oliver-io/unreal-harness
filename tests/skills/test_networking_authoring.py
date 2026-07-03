"""Skill-test loop TASK-2 — /networking: bp_set_class_replication parity.

The /networking skill (`.claude/skills/networking/SKILL.md`) tells small
sessions to "set `bAlwaysRelevant` on must-see actors" and generally advises
in terms of the native AActor replication flags. `bp_set_class_replication`
is the one authoring primitive the harness exposes for those flags and the
one the skill's advice funnels into — this battery pins that the primitive
actually lands the flags it names on the Blueprint class defaults.

Handler ground truth (read this task):
  `MCPBlueprintCommands.cpp:1460-1549` (HandleSetClassReplication) —
  `replicates` → `AActor::SetReplicates` (keeps RemoteRole consistent),
  `replicate_movement` → `SetReplicateMovement`, `always_relevant` →
  `bAlwaysRelevant` direct write, `net_cull_distance_squared` →
  `SetNetCullDistanceSquared` (public field access is UE_DEPRECATED(5.5)).
Engine ground truth (UE 5.7 source, read this task):
  `GameFramework/Actor.h:300` bAlwaysRelevant, `:318` bReplicateMovement,
  `:556` bReplicates (all `EditDefaultsOnly, Category=Replication`), `:871`
  NetCullDistanceSquared (public access deprecated at :869). Defaults from
  `AActor::InitializeDefaults` (Actor.cpp:287 `bReplicates = false`, :310
  `SetNetCullDistanceSquared(225000000.0f)`); bAlwaysRelevant and
  bReplicateMovement are bitfields left untouched there → default False.

Arrange/observe split: arrange with `bp_set_class_replication`; observe by
SPAWNING an instance of the compiled class (`actor_spawn` resolves the asset
path and appends `_C` — MCPSceneCommands.cpp:343) and reading each flag off
the instance with the non-mutating reflective export — `actor_set_property`
in `dry_run` mode returns the leaf's `before` text without writing
(MCPEditorCommands.cpp:645-690; the TASK-1 pattern). A freshly spawned actor
initializes from the CDO, so the instance read observes exactly the class
default the mutator claims to have set — through a different primitive, and
through the value the game would actually run with.

(Observability note, verified this task: `reflection_class_properties` /
`class_inspect` CANNOT resolve a Blueprint-generated class — their
ResolveClass uses `FindFirstObject<UClass>(..., EFindFirstObjectOptions::
ExactClass)`, which excludes `UBlueprintGeneratedClass` instances, and the
asset-path branch only exists for /Script/ paths (MCPReflectionCommands.cpp:
57-94, MCPBlueprintCommands.cpp:5543-5564). Hence the spawn-an-instance
observer.)

Headless-safe (no render/gui_only markers), bounded, self-cleaning: the one
Blueprint and every spawned instance are deleted in teardown / finally (and
the session-end Content/__MCPTest__ disk wipe backstops the asset).
`bp_set_class_replication`, `bp_create_blueprint`, and `bp_compile` are all
PIE-blocked on both sides (gates.ts:44-48, MCPCommonUtils.cpp:211-216), so if
PIE is live on the shared editor we skip rather than collide with someone
else's session.
"""

import time

import pytest

from harness.mcp_client import MCPToolError

NS = "/Game/__MCPTest__/skills_networking"
BP_PATH = f"{NS}/BP_MCPSkillNetSample"
ACTOR_BASENAME = "MCPSkillTest_NetSample"


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


def _delete_actor_if_present(client, name):
    try:
        client.command("actor_delete", {"name": name})
    except Exception:
        pass


def _retry_busy(fn, attempts=3, delay=2.0):
    """Bounded retry for the shared live editor's transient `engine_busy`
    (bridge receive timeout — e.g. a post-compile reinstancing/GC hitch, or
    another agent's long op holding the game thread). ONLY for steps that are
    idempotent by construction here: dry-run reads, spawn-after-delete-by-name,
    re-setting the same flag values, and recompiling the same Blueprint (the
    bridge executes commands sequentially on the game thread, so a retry after
    a timed-out-but-landed first attempt just converges to the same state).
    Bounded: <= attempts tries, fixed short delay."""
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


def _reflective_read(mcp, actor, prop, probe_value):
    """Non-mutating reflective read: actor_set_property dry_run returns the
    leaf's exported 'before' text without writing (MCPEditorCommands.cpp:647).
    probe_value must be type-compatible (the dry-run also validates the
    conversion) but is never committed."""
    result = _retry_busy(lambda: mcp.expect("actor_set_property", {
        "name": actor, "property": prop, "value": probe_value, "dry_run": True}))
    assert result.get("dry_run") is True, result
    entry = result["diff"]["properties_changed"][0]
    assert entry["property"] == prop, entry
    return entry["before"]


def _read_flags(mcp, actor):
    """The four native AActor flags, off a live instance (CDO-initialized)."""
    def as_bool(text):
        low = str(text).strip().lower()
        assert low in ("true", "false"), text
        return low == "true"

    return {
        "bReplicates": as_bool(_reflective_read(mcp, actor, "bReplicates", True)),
        "bAlwaysRelevant": as_bool(_reflective_read(mcp, actor, "bAlwaysRelevant", True)),
        "bReplicateMovement": as_bool(_reflective_read(mcp, actor, "bReplicateMovement", True)),
        "NetCullDistanceSquared": float(_reflective_read(
            mcp, actor, "NetCullDistanceSquared", 1.0)),
    }


def _spawn_instance(mcp, name):
    """Spawn an instance of the sample BP class; returns the actor name.
    actor_spawn resolves '/Game/...' asset paths by appending _C
    (MCPSceneCommands.cpp:343)."""
    def attempt():
        # Delete-then-spawn inside the retry unit keeps a retried spawn
        # idempotent even if the timed-out first spawn actually landed.
        _delete_actor_if_present(mcp, name)
        return mcp.expect("actor_spawn", {"class_path": BP_PATH, "name": name})

    spawned = _retry_busy(attempt)
    return spawned["actor"]["name"]


# ── fixture ──────────────────────────────────────────────────────────────────

@pytest.fixture(scope="module")
def net_bp(_mcp_client):
    """One Actor Blueprint for the module, compiled so GeneratedClass exists
    (the handler requires it — MCPBlueprintCommands.cpp:1472-1477). Deleted in
    teardown; the shared editor may outlive this run."""
    if _pie_is_running(_mcp_client):
        pytest.skip("PIE is running on the shared editor — bp_set_class_replication "
                    "and bp_create_blueprint are PIE-blocked (gates.ts / "
                    "MCPCommonUtils.cpp blocklists); not stopping someone else's session")
    _delete_bp_if_present(_mcp_client)
    _mcp_client.expect("bp_create_blueprint", {"name": BP_PATH, "parent_class": "Actor"})
    _mcp_client.expect("bp_compile", {"blueprint_name": BP_PATH})
    try:
        yield BP_PATH
    finally:
        _delete_bp_if_present(_mcp_client)


# ── 1. flag round-trip: mutate via one primitive, observe via another ────────

def test_class_replication_flags_round_trip(mcp, net_bp):
    """Each field the tool names lands on the real native AActor flag the
    /networking skill reasons about. Baseline first (a fresh Actor BP inherits
    the AActor defaults: bReplicates / bAlwaysRelevant / bReplicateMovement all
    False, NetCullDistanceSquared 225000000 — Actor.cpp:287/:310), then flip
    every flag to a NON-default value and observe each on a freshly spawned
    instance of the class."""
    # Baseline: an instance spawned BEFORE the mutation carries the AActor
    # defaults, so the post-set observations below can only pass if the
    # mutation actually reached the class defaults.
    actor = _spawn_instance(mcp, f"{ACTOR_BASENAME}_Before")
    try:
        before = _read_flags(mcp, actor)
    finally:
        _delete_actor_if_present(mcp, actor)
    assert before["bReplicates"] is False, before
    assert before["bAlwaysRelevant"] is False, before
    assert before["bReplicateMovement"] is False, before
    assert before["NetCullDistanceSquared"] == pytest.approx(225000000.0, rel=1e-6), before

    res = _retry_busy(lambda: mcp.expect("bp_set_class_replication", {
        "blueprint_name": net_bp,
        "replicates": True,
        "always_relevant": True,
        "replicate_movement": True,
        "net_cull_distance_squared": 12250000.0,  # 3500 uu, squared — non-default
    }))
    applied = res.get("applied") or {}
    assert applied.get("replicates") is True, res
    assert applied.get("always_relevant") is True, res
    assert applied.get("replicate_movement") is True, res

    # Observe through the independent instance read, not the mutator's echo.
    actor = _spawn_instance(mcp, f"{ACTOR_BASENAME}_After")
    try:
        after = _read_flags(mcp, actor)
    finally:
        _delete_actor_if_present(mcp, actor)
    assert after["bReplicates"] is True, after
    assert after["bAlwaysRelevant"] is True, after
    assert after["bReplicateMovement"] is True, after
    assert after["NetCullDistanceSquared"] == pytest.approx(12250000.0, rel=1e-6), after


def test_class_replication_survives_recompile(mcp, net_bp):
    """The flags must survive a full structural recompile (reinstancing copies
    the old CDO's values onto the regenerated class) and the Blueprint must
    still compile clean — otherwise the skill's 'set bAlwaysRelevant on the
    Blueprint' advice would silently evaporate on the next compile.
    Self-contained: arranges its own values rather than relying on test order."""
    _retry_busy(lambda: mcp.expect("bp_set_class_replication", {
        "blueprint_name": net_bp,
        "replicates": True,
        "always_relevant": True,
    }))
    _retry_busy(lambda: mcp.expect("bp_compile", {"blueprint_name": net_bp}))

    actor = _spawn_instance(mcp, f"{ACTOR_BASENAME}_Recompiled")
    try:
        flags = _read_flags(mcp, actor)
    finally:
        _delete_actor_if_present(mcp, actor)
    assert flags["bReplicates"] is True, flags
    assert flags["bAlwaysRelevant"] is True, flags
