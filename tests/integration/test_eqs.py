"""EQS (Environment Query System) domain — author a UEnvQuery asset, add
generator options and scoring/filtering tests, mutate generator/test properties,
read the structure back, and remove tests/options. Driven through the real MCP
server (the `mcp` fixture calls tools by name; the tool layer maps kwargs→bridge
params, etc.).

Self-contained: EQS queries are authorable from scratch via the AIModule asset
factory, so no imported content is required and nothing here is gui_only — every
op runs against the asset object graph, not an open editor window. A
module-scoped sample query is created once under the test namespace and reused
by the additive tests (each adds its own option and references it by the
returned index, so the tests stay independent of one another's ordering); the
remove tests build their own throwaway assets so option/test counts can be
asserted exactly. The session-end disk wipe (Content/__MCPTest__) cleans up.

Pattern for every test: arrange prerequisite state (idempotently — every create
is preceded by ensure_absent so the suite re-runs against a long-lived editor)
-> dispatch the op (raises on a non-success envelope) -> assert the resulting
state via read_eqs_query.

NOTE — tool names and kwarg keys are taken straight from the MCP tool layer
(tests/harness/tool_schemas.json is the authority): asset_path, generator_type,
test_type, option_index, test_index, target ("generator" or a numeric test-index
string), property_name, property_value, base_class. Generator/test type names
may be passed either as the short name (e.g. "ActorsOfClass") or the full class
name (e.g. "EnvQueryGenerator_ActorsOfClass"); list_eqs_types reports the full
class name in `class_name`, and read_eqs_query / add_* echo that full name back,
so the assertions compare against the full class name discovered from
list_eqs_types.
"""

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready

NS = "/Game/__MCPTest__/eqs"
SAMPLE = f"{NS}/EQS_Sample"

# Cache the discovered generator/test class names across the module so the
# introspection op is hit once rather than per test.
_DISCOVERY: dict = {}


def _types(bridge, base_class):
    result = bridge.expect("eqs_list_types", {"base_class": base_class})
    return [t for t in (result.get("types") or []) if isinstance(t, dict)]


def _pick_generator(bridge) -> str:
    """Discover a usable generator class (full class name). Prefer a simple,
    always-registered one; fall back to whatever the editor exposes first."""
    if "generator" in _DISCOVERY:
        return _DISCOVERY["generator"]
    names = [t.get("class_name") for t in _types(bridge, "generator")]
    chosen = next((f"EnvQueryGenerator_{p}"
                   for p in ("ActorsOfClass", "SimpleGrid", "OnCircle", "Donut")
                   if f"EnvQueryGenerator_{p}" in names), None)
    chosen = chosen or (names[0] if names else "EnvQueryGenerator_ActorsOfClass")
    _DISCOVERY["generator"] = chosen
    return chosen


def _pick_test(bridge) -> str:
    """Discover a usable test class (full class name)."""
    if "test" in _DISCOVERY:
        return _DISCOVERY["test"]
    names = [t.get("class_name") for t in _types(bridge, "test")]
    chosen = next((f"EnvQueryTest_{p}"
                   for p in ("Distance", "Dot", "Trace", "Random")
                   if f"EnvQueryTest_{p}" in names), None)
    chosen = chosen or (names[0] if names else "EnvQueryTest_Distance")
    _DISCOVERY["test"] = chosen
    return chosen


def _option(read_result, index):
    """Return the option dict at `index` from a read_eqs_query result, or None."""
    for opt in read_result.get("options") or []:
        if isinstance(opt, dict) and opt.get("index") == index:
            return opt
    return None


def _settable_scalar(props):
    """Pick a trivially-settable scalar from a serialized property list
    ([{name,type,value}, ...]) returning (name, value, expected_token) or None.
    expected_token is a lowercase substring that must appear in the property's
    re-read exported value. Prefers FString/FName (cleanest ImportText round-
    trip), then bool, then integer, then float; skips struct/enum/object props
    whose text import is finicky. The base UEnvQueryGenerator always exposes an
    editable FString `OptionName`, so a generator always yields a hit."""
    order = {"FString": 0, "FName": 0, "bool": 1,
             "int32": 2, "int64": 2, "int": 2, "uint8": 2, "uint32": 2,
             "float": 3, "double": 3}
    best = None
    best_rank = 99
    for p in props:
        if not isinstance(p, dict):
            continue
        name, cpp = p.get("name"), p.get("type")
        if not name or cpp not in order or order[cpp] >= best_rank:
            continue
        if cpp in ("FString", "FName"):
            best = (name, "MCPTestValue", "mcptestvalue")
        elif cpp == "bool":
            best = (name, True, "true")
        elif cpp in ("int32", "int64", "int", "uint8", "uint32"):
            best = (name, 7, "7")
        else:  # float/double
            best = (name, 1.5, "1.5")
        best_rank = order[cpp]
    return best


@pytest.fixture(scope="module")
def sample_eqs(_mcp_client):
    """Create one EQS query asset for the whole module."""
    ensure_absent(_mcp_client, SAMPLE)
    _mcp_client.expect("eqs_create", {"asset_path": SAMPLE})
    return SAMPLE


# ── introspection ───────────────────────────────────────────────────────────

@covers("eqs_list_types")
def test_list_eqs_types(mcp):
    result = mcp.expect("eqs_list_types", {"base_class": "all"})
    types = result.get("types")
    assert isinstance(types, list) and types, result
    assert result.get("count") == len(types), result
    cats = {t.get("category") for t in types if isinstance(t, dict)}
    assert {"generator", "test"} & cats, cats


@covers("eqs_list_types")
def test_list_eqs_types_filtered_to_generators(mcp):
    result = mcp.expect("eqs_list_types", {"base_class": "generator"})
    types = result.get("types") or []
    assert types, result
    assert all(t.get("category") == "generator" for t in types), types


# ── creation: the asset lands on disk ───────────────────────────────────────

@covers("eqs_create")
def test_create_eqs_query_writes_uasset_on_disk(mcp):
    path = f"{NS}/EQS_Created"
    ensure_absent(mcp, path)
    result = mcp.expect("eqs_create", {"asset_path": path})
    assert result.get("success") is True, result
    # create_eqs_query saves the package itself, so the .uasset must exist now.
    disk = config.uasset_disk_path(path)
    assert disk.is_file(), f"expected {disk} to exist after create_eqs_query"


# ── options (generators) ────────────────────────────────────────────────────

@covers("eqs_option_add", "eqs_read")
def test_add_option_then_read(mcp, sample_eqs):
    gen = _pick_generator(mcp)
    added = mcp.expect("eqs_option_add", {"asset_path": sample_eqs, "generator_type": gen})
    idx = added.get("option_index")
    assert isinstance(idx, int) and idx >= 0, added
    assert added.get("generator_type") == gen, added

    read = mcp.expect("eqs_read", {"asset_path": sample_eqs})
    opt = _option(read, idx)
    assert opt, f"option {idx} missing from {read.get('options')}"
    assert (opt.get("generator") or {}).get("type") == gen, opt


@covers("eqs_option_add", "eqs_read")
def test_add_option_dry_run_does_not_mutate(mcp, sample_eqs):
    gen = _pick_generator(mcp)
    before = mcp.expect("eqs_read", {"asset_path": sample_eqs})
    before_count = len(before.get("options") or [])

    result = mcp.expect("eqs_option_add", {
        "asset_path": sample_eqs, "generator_type": gen, "dry_run": True,
    })
    assert result.get("dry_run") is True, result
    diff = result.get("diff") or {}
    entry = (diff.get("options_added") or [{}])[0]
    assert entry.get("would_be_index") == before_count, entry
    assert entry.get("generator_type") == gen, entry

    after = mcp.expect("eqs_read", {"asset_path": sample_eqs})
    assert len(after.get("options") or []) == before_count, "dry_run added an option"


# ── tests (scoring / filtering) ─────────────────────────────────────────────

@covers("eqs_test_add", "eqs_option_add", "eqs_read")
def test_add_test_then_read(mcp, sample_eqs):
    gen = _pick_generator(mcp)
    test_type = _pick_test(mcp)
    opt_idx = mcp.expect("eqs_option_add",
                         {"asset_path": sample_eqs, "generator_type": gen})["option_index"]

    added = mcp.expect("eqs_test_add", {
        "asset_path": sample_eqs, "option_index": opt_idx, "test_type": test_type,
    })
    tidx = added.get("test_index")
    assert isinstance(tidx, int) and tidx >= 0, added
    assert added.get("test_type") == test_type, added

    read = mcp.expect("eqs_read", {"asset_path": sample_eqs})
    opt = _option(read, opt_idx)
    assert opt, read
    tests = opt.get("tests") or []
    match = next((t for t in tests if t.get("index") == tidx), None)
    assert match and match.get("type") == test_type, tests


# ── property mutation ───────────────────────────────────────────────────────

@covers("eqs_set_property", "eqs_option_add", "eqs_read")
def test_set_generator_property(mcp, sample_eqs):
    gen = _pick_generator(mcp)
    opt_idx = mcp.expect("eqs_option_add",
                         {"asset_path": sample_eqs, "generator_type": gen})["option_index"]

    # Discover a settable scalar from the generator's live properties.
    read = mcp.expect("eqs_read", {"asset_path": sample_eqs})
    gen_props = ((_option(read, opt_idx) or {}).get("generator") or {}).get("properties") or []
    chosen = _settable_scalar(gen_props)
    assert chosen, f"generator {gen} exposed no settable scalar property: {gen_props}"
    name, value, token = chosen

    result = mcp.expect("eqs_set_property", {
        "asset_path": sample_eqs, "option_index": opt_idx,
        "target": "generator", "property_name": name, "property_value": value,
    })
    assert result.get("success") is True, result

    # Read back: the named property's exported value must reflect the write.
    read2 = mcp.expect("eqs_read", {"asset_path": sample_eqs})
    props2 = ((_option(read2, opt_idx) or {}).get("generator") or {}).get("properties") or []
    entry = next((p for p in props2 if p.get("name") == name), None)
    assert entry, f"property {name} vanished from {props2}"
    assert token in str(entry.get("value")).lower(), entry


@covers("eqs_set_property", "eqs_option_add", "eqs_test_add", "eqs_read")
def test_set_test_property(mcp, sample_eqs):
    gen = _pick_generator(mcp)
    test_type = _pick_test(mcp)
    opt_idx = mcp.expect("eqs_option_add",
                         {"asset_path": sample_eqs, "generator_type": gen})["option_index"]
    tidx = mcp.expect("eqs_test_add", {
        "asset_path": sample_eqs, "option_index": opt_idx, "test_type": test_type,
    })["test_index"]

    read = mcp.expect("eqs_read", {"asset_path": sample_eqs})
    test_entry = next((t for t in (_option(read, opt_idx) or {}).get("tests") or []
                       if t.get("index") == tidx), None)
    assert test_entry, read
    chosen = _settable_scalar(test_entry.get("properties") or [])
    if not chosen:
        pytest.skip(f"test {test_type} exposed no trivially-settable scalar property")
    name, value, token = chosen

    # target is the numeric test index passed as a string (handler Atoi-parses it).
    result = mcp.expect("eqs_set_property", {
        "asset_path": sample_eqs, "option_index": opt_idx,
        "target": str(tidx), "property_name": name, "property_value": value,
    })
    assert result.get("success") is True, result

    read2 = mcp.expect("eqs_read", {"asset_path": sample_eqs})
    test2 = next((t for t in (_option(read2, opt_idx) or {}).get("tests") or []
                  if t.get("index") == tidx), None)
    entry = next((p for p in (test2 or {}).get("properties") or [] if p.get("name") == name), None)
    assert entry, f"property {name} vanished from {test2}"
    assert token in str(entry.get("value")).lower(), entry


# ── removal (own assets so counts assert exactly) ───────────────────────────

@covers("eqs_test_remove", "eqs_option_add", "eqs_test_add", "eqs_read",
        "eqs_create")
def test_remove_test(mcp):
    path = f"{NS}/EQS_RemoveTest"
    ensure_absent(mcp, path)
    mcp.expect("eqs_create", {"asset_path": path})
    gen = _pick_generator(mcp)
    test_type = _pick_test(mcp)
    mcp.expect("eqs_option_add", {"asset_path": path, "generator_type": gen})
    # Two tests on option 0; remove the first, the second must survive.
    mcp.expect("eqs_test_add", {"asset_path": path, "option_index": 0, "test_type": test_type})
    mcp.expect("eqs_test_add", {"asset_path": path, "option_index": 0, "test_type": test_type})

    removed = mcp.expect("eqs_test_remove", {
        "asset_path": path, "option_index": 0, "test_index": 0,
    })
    assert removed.get("success") is True, removed
    assert_ready(mcp)

    read = mcp.expect("eqs_read", {"asset_path": path})
    opt = _option(read, 0)
    assert opt, read
    assert len(opt.get("tests") or []) == 1, opt


@covers("eqs_option_remove", "eqs_option_add", "eqs_read", "eqs_create")
def test_remove_option(mcp):
    path = f"{NS}/EQS_RemoveOption"
    ensure_absent(mcp, path)
    mcp.expect("eqs_create", {"asset_path": path})
    gen = _pick_generator(mcp)
    mcp.expect("eqs_option_add", {"asset_path": path, "generator_type": gen})
    assert len(mcp.expect("eqs_read", {"asset_path": path}).get("options") or []) == 1

    removed = mcp.expect("eqs_option_remove", {"asset_path": path, "option_index": 0})
    assert removed.get("success") is True, removed
    assert_ready(mcp)

    read = mcp.expect("eqs_read", {"asset_path": path})
    assert len(read.get("options") or []) == 0, read


@covers("eqs_option_remove", "eqs_option_add", "eqs_read", "eqs_create")
def test_remove_option_dry_run_does_not_mutate(mcp):
    path = f"{NS}/EQS_RemoveOptionDry"
    ensure_absent(mcp, path)
    mcp.expect("eqs_create", {"asset_path": path})
    gen = _pick_generator(mcp)
    mcp.expect("eqs_option_add", {"asset_path": path, "generator_type": gen})

    result = mcp.expect("eqs_option_remove", {
        "asset_path": path, "option_index": 0, "dry_run": True,
    })
    assert result.get("dry_run") is True, result
    entry = ((result.get("diff") or {}).get("options_removed") or [{}])[0]
    assert entry.get("option_index") == 0, entry
    assert entry.get("generator_type") == gen, entry

    # The option must still be present after a dry-run remove.
    read = mcp.expect("eqs_read", {"asset_path": path})
    assert len(read.get("options") or []) == 1, read
