"""Niagara domain — build a system from scratch, then mutate and read it back.

Pattern (per the suite convention): a module-scoped sample Niagara System is
created once under the test namespace and reused. Each test arranges any extra
prerequisite state it needs (its own emitter, a renderer, a user parameter),
dispatches the op under test (``mcp.expect`` raises on a non-success
envelope), then asserts the resulting state via a read/inspect op.

Bridge command names and param keys here mirror the C++ handlers
(MCPNiagaraCommands*.cpp) exactly — NOT the Python tool kwarg names.
Notably the structural ops echo the post-uniquify emitter name back under the
key ``emitter_name`` (niagara_emitter_add) / ``emitter`` (everything else), and
read_niagara_system reports emitters under ``emitters`` (each: name/id/enabled).

The session-end disk wipe (Content/__MCPTest__) cleans up everything created.
"""

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent

NS = "/Game/__MCPTest__/niagara"
SAMPLE = f"{NS}/NS_Sample"
MATERIAL = f"{NS}/M_Sample"

# A stock engine module guaranteed to exist wherever the Niagara plugin is
# enabled; adding it to ParticleSpawn yields rapid-iteration inputs to poke.
INIT_PARTICLE_MODULE = "/Niagara/Modules/Spawn/Initialization/InitializeParticle.InitializeParticle"

# Value keys (besides the system/emitter/parameter identifiers) that the value
# setters accept per Niagara type, used to build a valid set_* payload.
_VALUE_KEYS_FOR_TYPE = {
    "float": {"value": 1.0},
    "int32": {"value": 2},
    "bool": {"value": True},
    "vector2": {"x": 1.0, "y": 2.0},
    "vector3": {"x": 1.0, "y": 2.0, "z": 3.0},
    "vector4": {"x": 1.0, "y": 2.0, "z": 3.0, "w": 4.0},
    "linear_color": {"r": 0.5, "g": 0.25, "b": 0.75, "a": 1.0},
    "position": {"x": 1.0, "y": 2.0, "z": 3.0},
}


@pytest.fixture(scope="module")
def sample_system(_mcp_client):
    """One empty Niagara System plus a single baseline CPU emitter, reused by the
    read-oriented tests. Returns (system_path, emitter_name)."""
    ensure_absent(_mcp_client, SAMPLE)
    _mcp_client.expect("niagara_system_create", {"system_path": SAMPLE})
    added = _mcp_client.expect("niagara_emitter_add", {
        "system_path": SAMPLE,
        "emitter_name": "Sample01",
        "sim_target": "cpu",
    })
    # Structural ops echo the (possibly uniquified) name back under emitter_name.
    emitter_name = added.get("emitter_name") or "Sample01"
    return SAMPLE, emitter_name


@pytest.fixture(scope="module")
def sample_material(_mcp_client):
    """A trivial material to bind onto sprite/ribbon renderers."""
    ensure_absent(_mcp_client, MATERIAL)
    _mcp_client.expect("material_create", {"material_path": MATERIAL})
    return MATERIAL


def _add_emitter(bridge, system_path, name):
    """Add a fresh emitter and return its resolved (post-uniquify) name. Lets each
    mutation test own its emitter, so tests stay order-independent."""
    res = bridge.expect("niagara_emitter_add", {
        "system_path": system_path,
        "emitter_name": name,
        "sim_target": "cpu",
    })
    return res.get("emitter_name") or name


# ── create / list / read ─────────────────────────────────────────────────────

@covers("niagara_system_create")
def test_create_system_writes_uasset_on_disk(mcp):
    path = f"{NS}/NS_Created"
    ensure_absent(mcp, path)
    result = mcp.expect("niagara_system_create", {"system_path": path})
    assert result.get("success") is True
    assert result.get("emitter_count") == 0, result
    # The create handler SaveAsset's the package, so the file must be on disk.
    disk = config.uasset_disk_path(path)
    assert disk.is_file(), f"expected {disk} to exist after niagara_system_create"


@covers("niagara_system_read")
def test_read_niagara_system(mcp, sample_system):
    system_path, emitter_name = sample_system
    result = mcp.expect("niagara_system_read", {"system_path": system_path})
    names = [e.get("name") for e in result.get("emitters", [])]
    assert emitter_name in names, result


@covers("niagara_list_systems")
def test_list_niagara_systems(mcp, sample_system):
    system_path, _ = sample_system
    result = mcp.expect("niagara_list_systems", {"path_filter": NS})
    paths = [s.get("path") for s in result.get("systems", [])]
    blob = str(paths)
    assert "NS_Sample" in blob, result


# ── emitters ─────────────────────────────────────────────────────────────────

@covers("niagara_emitter_add", "niagara_emitter_read")
def test_emitter_add_then_read(mcp, sample_system):
    system_path, _ = sample_system
    name = _add_emitter(mcp, system_path, "Extra01")
    result = mcp.expect("niagara_emitter_read", {
        "system_path": system_path,
        "emitter_name": name,
    })
    assert result.get("name") == name, result


@covers("niagara_emitter_set_enabled")
def test_set_emitter_enabled(mcp, sample_system):
    system_path, _ = sample_system
    name = _add_emitter(mcp, system_path, "Toggle01")
    mcp.expect("niagara_emitter_set_enabled", {
        "system_path": system_path,
        "emitter_name": name,
        "enabled": False,
    })
    # Confirm the persisted state via a read of the whole system.
    sysinfo = mcp.expect("niagara_system_read", {"system_path": system_path})
    match = next((e for e in sysinfo.get("emitters", []) if e.get("name") == name), None)
    assert match is not None, sysinfo
    assert match.get("enabled") is False, match


# ── renderers ────────────────────────────────────────────────────────────────

@covers("niagara_emitter_add_renderer")
def test_emitter_add_renderer(mcp, sample_system, sample_material):
    system_path, _ = sample_system
    name = _add_emitter(mcp, system_path, "Rend01")
    result = mcp.expect("niagara_emitter_add_renderer", {
        "system_path": system_path,
        "emitter_name": name,
        "renderer_type": "sprite",
        "material_path": sample_material,
    })
    assert result.get("renderer_type") == "sprite", result
    assert result.get("renderer_index") == 0, result


@covers("niagara_renderer_set_material")
def test_renderer_set_material(mcp, sample_system, sample_material):
    system_path, _ = sample_system
    name = _add_emitter(mcp, system_path, "RendMat01")
    mcp.expect("niagara_emitter_add_renderer", {
        "system_path": system_path,
        "emitter_name": name,
        "renderer_type": "sprite",
    })
    result = mcp.expect("niagara_renderer_set_material", {
        "system_path": system_path,
        "emitter_name": name,
        "renderer_index": 0,
        "material_path": sample_material,
    })
    assert "M_Sample" in str(result.get("material")), result


@covers("niagara_renderer_set_material_binding", "niagara_user_parameter_add")
def test_renderer_set_material_binding(mcp, sample_system):
    system_path, _ = sample_system
    name = _add_emitter(mcp, system_path, "RendBind01")
    # The renderer's MaterialUserParamBinding needs a Material-typed User.* slot.
    mcp.expect("niagara_user_parameter_add", {
        "system_path": system_path,
        "parameter_name": "RibbonMaterial",
        "type_name": "material",
    })
    mcp.expect("niagara_emitter_add_renderer", {
        "system_path": system_path,
        "emitter_name": name,
        "renderer_type": "sprite",
    })
    result = mcp.expect("niagara_renderer_set_material_binding", {
        "system_path": system_path,
        "emitter_name": name,
        "renderer_index": 0,
        "user_param_name": "RibbonMaterial",
    })
    assert "RibbonMaterial" in str(result.get("user_param_name")), result


# ── modules ──────────────────────────────────────────────────────────────────

@covers("niagara_module_add", "niagara_module_get_inputs", "niagara_module_set_input")
def test_module_add_inputs_and_set(mcp, sample_system):
    system_path, _ = sample_system
    name = _add_emitter(mcp, system_path, "Mod01")

    add = mcp.expect("niagara_module_add", {
        "system_path": system_path,
        "emitter_name": name,
        "target_usage": "ParticleSpawn",
        "module_script_path": INIT_PARTICLE_MODULE,
    })
    assert add.get("module_node_created") is True, add

    inputs = mcp.expect("niagara_module_get_inputs", {
        "system_path": system_path,
        "emitter_name": name,
    })
    module_inputs = inputs.get("module_inputs", [])
    if not module_inputs:
        pytest.skip("module added but no rapid-iteration inputs were exposed to set")

    # Pick the first input whose type we know how to write a value for.
    target = next((mi for mi in module_inputs
                   if mi.get("type") in _VALUE_KEYS_FOR_TYPE), None)
    if target is None:
        pytest.skip(f"no settable scalar/vector module input among: "
                    f"{[mi.get('type') for mi in module_inputs]}")

    payload = {
        "system_path": system_path,
        "emitter_name": name,
        "parameter_name": target["parameter_name"],
    }
    payload.update(_VALUE_KEYS_FOR_TYPE[target["type"]])
    result = mcp.expect("niagara_module_set_input", payload)
    assert result.get("parameter_name") == target["parameter_name"], result


@covers("niagara_scratch_pad_module_add")
def test_add_scratch_pad_module(mcp, sample_system):
    system_path, _ = sample_system
    name = _add_emitter(mcp, system_path, "Scratch01")
    result = mcp.expect("niagara_scratch_pad_module_add", {
        "system_path": system_path,
        "emitter_name": name,
        "module_name": "MCPScratchDouble",
        "target_usage": "ParticleUpdate",
        "hlsl": "OutValue = InValue * 2.0;",
        "inputs": [{"name": "InValue", "type": "float"}],
        "outputs": [{"name": "OutValue", "type": "float"}],
    })
    assert result.get("success") is True, result
    assert result.get("module_node_created") is True, result


# ── user parameters ──────────────────────────────────────────────────────────

@covers("niagara_user_parameter_add", "niagara_user_parameter_set",
        "niagara_user_parameter_remove")
def test_user_parameter_lifecycle(mcp, sample_system):
    system_path, _ = sample_system
    pname = "MCPSpeed"

    mcp.expect("niagara_user_parameter_add", {
        "system_path": system_path,
        "parameter_name": pname,
        "type_name": "float",
    })
    # It should now appear (with the User. prefix) on a read.
    sysinfo = mcp.expect("niagara_system_read", {"system_path": system_path})
    assert any(pname in str(p.get("name")) for p in sysinfo.get("user_parameters", [])), sysinfo

    setres = mcp.expect("niagara_user_parameter_set", {
        "system_path": system_path,
        "parameter_name": pname,
        "value": 12.5,
    })
    assert setres.get("success") is True, setres

    rem = mcp.expect("niagara_user_parameter_remove", {
        "system_path": system_path,
        "parameter_name": pname,
    })
    assert pname in str(rem.get("removed_parameter")), rem


# ── standalone script asset ──────────────────────────────────────────────────

@covers("niagara_script_create")
def test_niagara_script_create(mcp):
    path = f"{NS}/NSC_Sample"
    ensure_absent(mcp, path)
    result = mcp.expect("niagara_script_create", {
        "usage": "module",
        "path": NS,
        "name": "NSC_Sample",
    })
    assert result.get("success") is not False, result
    assert "NSC_Sample" in str(result.get("asset_path")), result
