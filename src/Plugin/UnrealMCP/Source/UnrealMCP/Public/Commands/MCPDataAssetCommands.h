#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class UDataAsset;
class FProperty;

/**
 * Handler class for UDataAsset MCP commands.
 *
 * Fills the gap noted in docs/todo/MCP_PHILOSOPHY.md: the plugin had
 * create_blueprint / create_material / create_state_tree / etc. but no
 * way to author UDataAsset instances or edit their CDO properties from
 * MCP.  This class exposes a parsimonious three-command surface that
 * covers the full author / edit / inspect cycle for any UDataAsset
 * subclass — driven via reflection so adding a new UDataAsset C++ class
 * doesn't require any plugin work.
 *
 * Per the philosophy doc:
 *   • Auto-save on success (project's save-while-editing policy).
 *   • Edit-exposed (UPROPERTY) fields only — private state is rejected.
 *   • Arrays support set / append / clear / remove_at via an `action`
 *     parameter; non-array writes ignore `action` and just set.
 */
class FMCPDataAssetCommands
{
public:
	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	/** create_data_asset — instantiate a UDataAsset of the given class
	 *  at a /Game-relative path.  Mirrors create_blueprint's path and
	 *  class-resolution semantics. */
	TSharedPtr<FJsonObject> HandleCreateDataAsset(const TSharedPtr<FJsonObject>& Params);

	/** set_data_asset_property — write a single property on a data
	 *  asset's CDO.  For arrays, the optional `action` selects between
	 *  set (replace), append, clear, remove_at.  For non-arrays the
	 *  whole `value` is parsed via JsonValueToUProperty. */
	TSharedPtr<FJsonObject> HandleSetDataAssetProperty(const TSharedPtr<FJsonObject>& Params);

	/** read_data_asset — JSON dump of every editable property on the
	 *  asset.  Mirrors read_data_table's shape but works on any
	 *  UDataAsset subclass via reflection. */
	TSharedPtr<FJsonObject> HandleReadDataAsset(const TSharedPtr<FJsonObject>& Params);

	// ── Helpers ──────────────────────────────────────────────────────────────

	/** Resolve a UDataAsset subclass via three strategies (FQN path,
	 *  short name, U/A-prefix-stripped short name).  Returns nullptr if
	 *  no match or if the resolved class is not a UDataAsset. */
	static UClass* ResolveDataAssetClass(const FString& ClassName);

	/** Load a UDataAsset by /Game-relative path.  Falls back to LoadObject
	 *  with the .AssetName suffix appended if EditorAssetLibrary's
	 *  normalisation misses (mirrors FindMaterialByPath). */
	static UDataAsset* LoadDataAssetByPath(const FString& AssetPath);

	/** Locate an editable FProperty by name on the asset's class.
	 *  Returns nullptr if not found or if the property lacks edit flags
	 *  (CPF_Edit / CPF_BlueprintVisible) — guards against accidental
	 *  writes to private internals. */
	static FProperty* FindEditableProperty(UDataAsset* Asset, const FName& PropertyName);
};
