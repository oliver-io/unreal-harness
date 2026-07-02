"""GameplayTags domain — register a tag, list it back, rename it, and remove it.

These ops edit the project's gameplay-tag registry (persisted to
Config/DefaultGameplayTags.ini), not /Game content. The session disk-reset only
wipes Content/__MCPTest__, so each test removes the tags it creates (best-effort)
and every create is made idempotent by removing first — the suite stays
re-runnable against a long-lived editor.

Pattern: arrange (clear any prior state) -> dispatch the op -> assert via tag_list.
"""

from harness.coverage import covers

PREFIX = "MCPTest"
ALPHA = "MCPTest.Alpha"
BETA = "MCPTest.Beta"
GAMMA = "MCPTest.Gamma"


def _names(list_result) -> list:
    return [entry.get("tag") for entry in list_result.get("tags", [])]


def _drop(bridge, *tags) -> None:
    """Best-effort remove (idempotency / cleanup). Never raises."""
    for tag in tags:
        try:
            bridge.command("tag_remove", {"tag": tag, "force": True, "dry_run": False})
        except Exception:
            pass


@covers("tag_add", "tag_list", "tag_remove")
def test_tag_add_then_list(mcp):
    _drop(mcp, ALPHA)
    try:
        mcp.expect("tag_add", {"tag": ALPHA, "dry_run": False})
        listing = mcp.expect("tag_list", {"prefix": PREFIX, "include_dev_comments": True})
        assert ALPHA in _names(listing), listing
    finally:
        _drop(mcp, ALPHA)


@covers("tag_move", "tag_add", "tag_list")
def test_tag_move_renames(mcp):
    _drop(mcp, ALPHA, BETA)
    try:
        mcp.expect("tag_add", {"tag": ALPHA, "dry_run": False})
        mcp.expect("tag_move", {
            "from_tag": ALPHA,
            "to_tag": BETA,
            "rename_children": True,
            "dry_run": False,
        })
        listing = mcp.expect("tag_list", {"prefix": PREFIX, "include_dev_comments": True})
        names = _names(listing)
        assert BETA in names, listing
        # The old tag node is renamed out of the registry (a redirector is left,
        # but the source tag itself no longer enumerates).
        assert ALPHA not in names, listing
    finally:
        _drop(mcp, ALPHA, BETA)


@covers("tag_remove", "tag_add", "tag_list")
def test_tag_remove_then_gone(mcp):
    _drop(mcp, GAMMA)
    mcp.expect("tag_add", {"tag": GAMMA, "dry_run": False})
    # Sanity: it is present before removal.
    before = mcp.expect("tag_list", {"prefix": PREFIX, "include_dev_comments": True})
    assert GAMMA in _names(before), before

    mcp.expect("tag_remove", {"tag": GAMMA, "force": True, "dry_run": False})
    after = mcp.expect("tag_list", {"prefix": PREFIX, "include_dev_comments": True})
    assert GAMMA not in _names(after), after
