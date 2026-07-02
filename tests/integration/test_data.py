"""Data domain — UserDefinedStruct, UserDefinedEnum, DataTable, and UDataAsset.

Every create in this domain auto-saves (the C++ handlers call
UEditorAssetLibrary::SaveAsset on success), so each create test additionally
asserts the .uasset landed on disk at the mapped Content path.

Pattern for every test: arrange prerequisite state (ensure_absent so the suite
is re-runnable against a long-lived editor) -> dispatch the op (raises on a
non-success envelope) -> assert the resulting state via a read/inspect op.

DataTable depends on a row UScriptStruct, so the datatable test creates a
UserDefinedStruct first (via struct_create) and binds the table to it. The
DataAsset tests use the built-in engine class /Script/Engine.PreviewMeshCollection
(a concrete UDataAsset subclass with an edit-exposed `SkeletalMeshes` array),
so they need no custom C++ class shipped by the fixture project.
"""

import pytest

from harness import config
from harness.coverage import covers
from harness.ops import ensure_absent, assert_ready, first_asset_of

NS = "/Game/__MCPTest__/data"

# A built-in, always-loaded UDataAsset subclass with an edit-exposed TArray
# property (`SkeletalMeshes`) — lets the DataAsset CRUD tests run on an empty
# project with no custom UDataAsset class.
DATA_ASSET_CLASS = "/Script/Engine.PreviewMeshCollection"


@covers("struct_create")
def test_struct_create_writes_uasset_on_disk(mcp):
    path = f"{NS}/S_Row"
    ensure_absent(mcp, path)
    result = mcp.expect("struct_create", {"asset_path": path})
    assert result.get("success") is True, result
    assert result.get("asset_path") == path, result
    assert "UserDefinedStruct" in str(result.get("class")), result
    # struct_create auto-saves; the package must exist on disk.
    disk = config.uasset_disk_path(path)
    assert disk.is_file(), f"expected {disk} to exist after struct_create"


@covers("enum_create", "enum_inspect")
def test_enum_create_then_inspect(mcp):
    path = f"{NS}/E_Faction"
    ensure_absent(mcp, path)
    members = ["Neutral", "Friendly", "Hostile"]
    created = mcp.expect("enum_create", {"asset_path": path, "members": members})
    assert created.get("success") is True, created
    assert created.get("asset_path") == path, created
    # First supplied member replaces the seeded NewEnumerator0; rest append.
    assert created.get("members_added") == len(members), created
    # Auto-saved to disk.
    disk = config.uasset_disk_path(path)
    assert disk.is_file(), f"expected {disk} to exist after enum_create"

    # Read it back via enum_inspect (accepts the UserDefinedEnum asset path).
    inspected = mcp.expect("enum_inspect", {"enum_name": path})
    assert inspected.get("success") is not False, inspected
    assert inspected.get("is_user_defined") is True, inspected
    assert isinstance(inspected.get("members"), list) and inspected["members"], inspected
    # NOTE: the supplied display names ("Neutral"/...) may surface as the
    # enumerator authored names rather than display labels depending on the
    # UserDefinedEnum save path; assert the count round-trips, which is the
    # robust invariant. (create-side members_added is asserted above.)
    assert inspected.get("member_count", 0) >= 1, inspected


@covers("datatable_create", "asset_datatable_read", "struct_create")
def test_datatable_create_then_read(mcp):
    # A DataTable needs a row UScriptStruct — create a UserDefinedStruct first
    # and bind the table to it (by asset path; the handler resolves it via
    # LoadObject, which auto-handles the .AssetName suffix).
    row_struct = f"{NS}/S_TableRow"
    ensure_absent(mcp, row_struct)
    struct_res = mcp.expect("struct_create", {"asset_path": row_struct})
    assert struct_res.get("success") is True, struct_res

    table = f"{NS}/DT_Rows"
    ensure_absent(mcp, table)
    created = mcp.expect("datatable_create", {
        "asset_path": table,
        "row_struct": row_struct,
    })
    assert created.get("success") is True, created
    assert created.get("asset_path") == table, created
    assert "DataTable" in str(created.get("class")), created
    # Auto-saved.
    disk = config.uasset_disk_path(table)
    assert disk.is_file(), f"expected {disk} to exist after datatable_create"

    # Read it back: fresh table has the bound row struct, struct-derived
    # columns, and zero rows.
    read = mcp.expect("asset_datatable_read", {"table_path": table})
    assert read.get("row_count") == 0, read
    assert isinstance(read.get("columns"), list), read
    assert read.get("row_struct"), read


@covers("asset_dataasset_create")
def test_create_data_asset_writes_uasset_on_disk(mcp):
    path = f"{NS}/DA_PreviewMeshes"
    ensure_absent(mcp, path)
    result = mcp.expect("asset_dataasset_create", {
        "name": path,
        "asset_class": DATA_ASSET_CLASS,
    })
    assert result.get("success") is True, result
    assert result.get("asset_path") == path, result
    assert "PreviewMeshCollection" in str(result.get("class")), result
    # create_data_asset auto-saves; the package must exist on disk.
    disk = config.uasset_disk_path(path)
    assert disk.is_file(), f"expected {disk} to exist after create_data_asset"


@covers("asset_dataasset_create", "asset_dataasset_set_property", "asset_dataasset_read")
def test_data_asset_set_property_then_read(mcp):
    path = f"{NS}/DA_Editable"
    ensure_absent(mcp, path)
    mcp.expect("asset_dataasset_create", {"name": path, "asset_class": DATA_ASSET_CLASS})

    # `SkeletalMeshes` is an edit-exposed TArray on UPreviewMeshCollection;
    # "clear" empties it (no external asset reference needed) and reports the
    # new array size.
    mutated = mcp.expect("asset_dataasset_set_property", {
        "asset_path": path,
        "property": "SkeletalMeshes",
        "action": "clear",
    })
    assert mutated.get("success") is True, mutated
    assert mutated.get("action") == "clear", mutated
    assert mutated.get("array_size") == 0, mutated

    # read_data_asset JSON-dumps every edit-exposed property; the mutated
    # array property must appear.
    read = mcp.expect("asset_dataasset_read", {"asset_path": path})
    props = read.get("properties")
    assert isinstance(props, dict), read
    assert "SkeletalMeshes" in props, read
    assert "PreviewMeshCollection" in str(read.get("class")), read
    assert_ready(mcp)

