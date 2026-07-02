#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for level (UWorld / .umap) persistence driven from MCP.
 *
 * Closes GAP-006: before these commands there was no way to create / save / load a
 * level over the bridge, so the editor lived in the transient `/Temp/Untitled` world
 * that is discarded on restart. These four commands are the headless equivalents of
 * the File menu's New / Save / Save As / Open Level, implemented on top of the
 * engine's UEditorLoadingAndSavingUtils (UnrealEd):
 *
 *   level_new      → NewBlankMap / NewMapFromTemplate
 *   level_save     → SaveMap on the CURRENT world's existing on-disk package
 *   level_save_as  → SaveMap to a new /Game/... package path
 *   level_load     → LoadMap of an existing /Game/... package
 *
 * Notes for callers:
 *   - All four are mutators: blocked during PIE and refuse dry_run (no preview
 *     semantics — a level either lands on disk / becomes the active world or it
 *     doesn't). See IsBlockedDuringPie / IsBlockedFromDryRun in MCPCommonUtils.cpp.
 *   - level_save fails (invalid_path) on a transient `/Temp/` untitled world and
 *     directs the caller to level_save_as.
 *   - The "current world" is the editor world: GEditor->GetEditorWorldContext().World().
 */
class FMCPLevelCommands
{
public:
	FMCPLevelCommands();

	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	/**
	 * Create a new blank level (optionally from a template), replacing the current
	 * editor world. Does NOT save the outgoing world (bSaveExistingMap=false).
	 *
	 * Optional params:
	 *   template (string) — /Game/... path to a template level. Empty/omitted → blank map.
	 *
	 * Response on success: { created: true, map_name, package_path }
	 */
	TSharedPtr<FJsonObject> HandleNewLevel(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Save the CURRENT editor world to its existing on-disk package. Errors with
	 * invalid_path on a transient `/Temp/` untitled world (use level_save_as).
	 *
	 * Response on success: { saved: true, map_name, package_path }
	 */
	TSharedPtr<FJsonObject> HandleSaveLevel(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Save the CURRENT editor world to a new /Game/... content path.
	 *
	 * Required params:
	 *   package_path (string) — /Game/... package path (no extension), e.g. /Game/Maps/L_Arena
	 *
	 * Response on success: { saved: true, map_name, package_path }
	 */
	TSharedPtr<FJsonObject> HandleSaveLevelAs(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Open an existing level by /Game/... package path, replacing the current world.
	 *
	 * Required params:
	 *   package_path (string) — /Game/... package path to an existing UWorld.
	 *
	 * Response on success: { loaded: true, map_name, package_path }
	 */
	TSharedPtr<FJsonObject> HandleLoadLevel(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Bake the scene into every reflection capture's cubemap and write the result into
	 * the level's MapBuildDataRegistry — the headless equivalent of Build > Build
	 * Reflection Captures (UEditorEngine::BuildReflectionCaptures).
	 *
	 * Closes a real gap: the editor viewport renders a TRANSIENT live re-capture so it
	 * always looks current, but PIE / cooked builds read the SERIALIZED MapBuildData. If
	 * that is stale (e.g. after authoring/editing reflective materials over MCP) the
	 * reflection environment falls back to a flat grey ambient — hence the "looks great
	 * in editor, washed-out in PIE" divergence and the on-screen "REFLECTION CAPTURES
	 * NEED TO BE REBUILT" warning. Neither `UpdateReflectionCaptures` (console) nor moving
	 * the capture actor writes the baked data; only this build path does.
	 *
	 * Optional params:
	 *   save (bool, default true) — persist the dirtied MapBuildData (+ map) to disk so
	 *     PIE/cooked builds pick up the bake. Set false to bake without saving.
	 *
	 * Mutator: blocked during PIE and refuses dry_run (no preview — it either bakes or not).
	 *
	 * Response on success: { built: true, map_name, package_path, saved }
	 */
	TSharedPtr<FJsonObject> HandleBuildReflectionCaptures(const TSharedPtr<FJsonObject>& Params);
};
