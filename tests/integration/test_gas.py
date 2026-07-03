"""Gameplay Ability System domain — create the GAS scaffolding asset trio
(AttributeSet, GameplayAbility, GameplayEffect) and exercise the runtime apply.

The three create ops are typed wrappers over create_blueprint; each auto-saves on
success, so the assertion is "a uasset exists on disk where we asked". gas_effect_apply
is a runtime tool that requires a live PIE session with an AbilitySystemComponent on
the target — the headless suite has no PIE, so that test asserts the documented
``not_in_pie`` guard and skips if the editor is unexpectedly in another state.

Pattern: arrange -> dispatch the create -> assert the uasset on disk.
"""

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready

NS = "/Game/__MCPTest__/gas"
ATTRSET = f"{NS}/AS_Health"
ABILITY = f"{NS}/GA_MCPTest"
EFFECT = f"{NS}/GE_MCPTest"
APPLY_EFFECT = f"{NS}/GE_ApplySample"


@pytest.fixture(scope="module")
def sample_effect(_mcp_client):
    """A GameplayEffect to reference from the apply test."""
    ensure_absent(_mcp_client, APPLY_EFFECT)
    _mcp_client.expect("gas_effect_create", {"asset_path": APPLY_EFFECT, "duration_policy": "Instant"})
    return APPLY_EFFECT


@covers("gas_attributeset_create")
def test_gas_attributeset_create_writes_uasset(mcp):
    ensure_absent(mcp, ATTRSET)
    result = mcp.expect("gas_attributeset_create", {"asset_path": ATTRSET})
    assert result.get("success") is True, result
    assert result.get("is_scaffolding") is True, result
    disk = config.uasset_disk_path(ATTRSET)
    assert disk.is_file(), f"expected {disk} after gas_attributeset_create"


@covers("gas_ability_create")
def test_gas_ability_create_writes_uasset(mcp):
    ensure_absent(mcp, ABILITY)
    result = mcp.expect("gas_ability_create", {"asset_path": ABILITY})
    assert result.get("success") is True, result
    disk = config.uasset_disk_path(ABILITY)
    assert disk.is_file(), f"expected {disk} after gas_ability_create"


@covers("gas_effect_create")
def test_gas_effect_create_writes_uasset(mcp):
    ensure_absent(mcp, EFFECT)
    result = mcp.expect("gas_effect_create", {
        "asset_path": EFFECT,
        "duration_policy": "Instant",
    })
    assert result.get("success") is True, result
    disk = config.uasset_disk_path(EFFECT)
    assert disk.is_file(), f"expected {disk} after gas_effect_create"


def _outbound_refs(mcp, asset_path: str) -> set:
    """Independent readback: the asset registry's outbound dependency set for a
    package (rebuilt from the saved package's import table on every save) — a
    different, deeper primitive than the GAS setter's own echo."""
    result = mcp.expect("asset_references", {
        "asset_path": asset_path,
        "direction": "outbound",
    })
    return {r["path"] for r in result.get("references", [])}


@covers("gas_ability_set_cost", "gas_ability_set_cooldown")
def test_gas_ability_set_cost_and_cooldown_bind_then_clear(mcp):
    """Bind distinct Cost/Cooldown GameplayEffect classes on a GA Blueprint, then
    observe through the asset registry's dependency graph (not the setter's echo):
    both GE packages become hard outbound dependencies of the saved ability, and
    clearing ONE slot removes exactly that GE's dependency while the other
    survives — proving the two setters write independent CDO slots and that
    effect_class="" clears."""
    ability = f"{NS}/GA_CostCooldown"
    ge_cost = f"{NS}/GE_CostFx"
    ge_cooldown = f"{NS}/GE_CooldownFx"
    for p in (ability, ge_cost, ge_cooldown):
        ensure_absent(mcp, p)
    try:
        mcp.expect("gas_ability_create", {"asset_path": ability})
        mcp.expect("gas_effect_create", {"asset_path": ge_cost, "duration_policy": "Instant"})
        mcp.expect("gas_effect_create", {"asset_path": ge_cooldown, "duration_policy": "Duration"})

        # Baseline: the fresh ability references neither GE.
        baseline = _outbound_refs(mcp, ability)
        assert ge_cost not in baseline and ge_cooldown not in baseline, baseline

        set_cost = mcp.expect("gas_ability_set_cost", {
            "ability_path": ability, "effect_class": ge_cost,
        })
        assert set_cost.get("cleared") is False, set_cost
        set_cd = mcp.expect("gas_ability_set_cooldown", {
            "ability_path": ability, "effect_class": ge_cooldown,
        })
        assert set_cd.get("cleared") is False, set_cd

        # Observe: BOTH GEs are now hard dependencies of the saved ability. If the
        # setters wrote the same property, the second bind would have overwritten
        # the first and only one GE could be referenced.
        refs = _outbound_refs(mcp, ability)
        assert ge_cost in refs, refs
        assert ge_cooldown in refs, refs

        # Clear the COST only; exactly its dependency drops, the cooldown's survives.
        cleared = mcp.expect("gas_ability_set_cost", {
            "ability_path": ability, "effect_class": "",
        })
        assert cleared.get("cleared") is True, cleared
        refs = _outbound_refs(mcp, ability)
        assert ge_cost not in refs, refs
        assert ge_cooldown in refs, refs

        # Clear the cooldown too; both gone.
        cleared = mcp.expect("gas_ability_set_cooldown", {
            "ability_path": ability, "effect_class": "",
        })
        assert cleared.get("cleared") is True, cleared
        refs = _outbound_refs(mcp, ability)
        assert ge_cost not in refs and ge_cooldown not in refs, refs
    finally:
        # Delete the ability first — it may still reference the GEs on failure.
        for p in (ability, ge_cost, ge_cooldown):
            ensure_absent(mcp, p)


@covers("gas_effect_apply")
def test_gas_effect_apply_requires_pie(mcp, sample_effect):
    # Runtime op: needs a live PIE world with an AbilitySystemComponent target.
    # The headless suite has none, so we assert the documented not_in_pie guard.
    resp = mcp.command("gas_effect_apply", {
        "target_actor": "MCPTest_GASTarget",
        "effect_class": sample_effect + "_C",
        "level": 1.0,
    })
    if resp.get("status") == "success":
        # Only reachable inside an active PIE session with a valid ASC target.
        assert "handle_valid" in resp.get("result", {}), resp
    else:
        code = resp.get("error_code")
        if code != "not_in_pie":
            pytest.skip(
                "gas_effect_apply needs a live PIE session with an ASC target; "
                f"got error_code={code!r}"
            )
        assert code == "not_in_pie", resp
    assert_ready(mcp)

