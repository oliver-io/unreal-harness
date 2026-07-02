// Asset refactoring operations: rename, move, duplicate, save, list, fixup redirectors
// These operations use UE's Asset Tools / Editor Asset Library to properly
// update all internal references — never raw filesystem moves.

#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class UNREALMCP_API FAssetManager
{
public:
	/**
	 * Rename an asset in-place (same folder, new name).
	 * Creates a redirector at the old path so existing references keep working.
	 *
	 * Params: source_path (FString), new_name (FString)
	 */
	static TSharedPtr<FJsonObject> RenameAsset(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Move an asset to a different folder (optionally rename at the same time).
	 * Creates a redirector at the old path.
	 *
	 * Params: source_path (FString), destination_folder (FString),
	 *         new_name (FString, optional — keeps original name if omitted)
	 */
	static TSharedPtr<FJsonObject> MoveAsset(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Duplicate an asset to a new path.
	 *
	 * Params: source_path (FString), destination_path (FString)
	 */
	static TSharedPtr<FJsonObject> DuplicateAsset(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Save one or more assets to disk.
	 *
	 * Params: asset_paths (TArray<FString>)
	 *         If empty, saves all dirty assets.
	 */
	static TSharedPtr<FJsonObject> SaveAsset(const TSharedPtr<FJsonObject>& Params);

	/**
	 * List assets under a path (recursive).
	 *
	 * Params: directory_path (FString), recursive (bool, default true),
	 *         class_filter (FString, optional — e.g. "Blueprint")
	 */
	static TSharedPtr<FJsonObject> ListAssets(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Consolidate redirectors under a given path so stale redirector
	 * assets are removed and all references point directly to the new location.
	 *
	 * Params: directory_path (FString)
	 */
	static TSharedPtr<FJsonObject> FixupRedirectors(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Delete an asset from the project.
	 *
	 * Params: asset_path (FString)
	 *         force (bool, default false — if true, delete even if referenced)
	 */
	static TSharedPtr<FJsonObject> DeleteAsset(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Open an asset in its editor (like double-clicking in the Content Browser).
	 *
	 * Params: asset_path (FString) — e.g. "/Game/Materials/M_Landscape"
	 */
	static TSharedPtr<FJsonObject> OpenAsset(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Direction-aware lookup over the IAssetRegistry reference graph.
	 * Mirrors mcp/docs/todo/4_asset_references.md.
	 *
	 * Params:
	 *   asset_path     (FString, required) — package name like "/Game/Foo/Bar"
	 *                  or object path "/Game/Foo/Bar.Bar" (suffix stripped).
	 *   direction      (FString, required) — "outbound" | "inbound".
	 *                  Closed set; any other value returns invalid_argument.
	 *   depth          (int32,   optional, default 1, 1..10). Recursive BFS.
	 *   cursor         (int32,   optional, default 0). Pagination offset on
	 *                  the deduplicated, sorted result list.
	 *   limit          (int32,   optional, default 200, max 1000).
	 *   include_hard   (bool,    optional, default true).
	 *   include_soft   (bool,    optional, default true).
	 *   include_searchable_name (bool, optional, default true).
	 *
	 * Returns rows of { path, kind, direct } where kind is one of
	 * "hard", "soft", "searchable_name". `direct=true` for one-hop entries.
	 */
	static TSharedPtr<FJsonObject> AssetReferences(const TSharedPtr<FJsonObject>& Params);

private:
	static TSharedPtr<FJsonObject> CreateSuccessResponse(const TSharedPtr<FJsonObject>& Data = nullptr);

	/** Validate that a package path looks like a valid /Game/... path */
	static bool IsValidPackagePath(const FString& Path);
};
