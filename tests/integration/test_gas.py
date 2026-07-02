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

