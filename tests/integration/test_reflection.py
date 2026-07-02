"""Reflection domain — pure read-only introspection of the editor's live UClass
reflection database. No assets are created or mutated.

These exercise the three class-introspection ops against well-known native engine
classes (Actor / PointLight), which are always loaded, so the assertions hold in an
empty project.

Pattern: dispatch the read op -> assert the returned shape carries the expected fields.
"""

from harness.coverage import covers


@covers("class_query")
def test_class_query_finds_pointlight(mcp):
    result = mcp.expect("class_query", {"name_pattern": "PointLight"})
    classes = result.get("classes", [])
    assert isinstance(classes, list) and classes, result
    names = [c.get("name") for c in classes]
    assert any("PointLight" in (n or "") for n in names), names
    # Each entry carries the documented shape.
    assert all("path" in c and "name" in c for c in classes), classes


@covers("class_inspect")
def test_class_inspect_actor(mcp):
    result = mcp.expect("class_inspect", {
        "class_name": "Actor",
        "include": ["properties", "hierarchy"],
    })
    assert "actor" in str(result.get("class_path", "")).lower(), result
    props = result.get("properties", [])
    assert isinstance(props, list) and props, result
    assert all("name" in p for p in props), props
    # hierarchy is the parent chain inclusive of the class itself.
    assert result.get("hierarchy"), result


@covers("reflection_class_properties")
def test_get_class_properties_actor(mcp):
    result = mcp.expect("reflection_class_properties", {
        "class_name": "Actor",
        "include_inherited": False,
    })
    assert "actor" in str(result.get("class_name", "")).lower() \
        or "actor" in str(result.get("class_path", "")).lower(), result
    props = result.get("properties", [])
    assert isinstance(props, list) and props, result
    assert all("name" in p and "type" in p for p in props), props
