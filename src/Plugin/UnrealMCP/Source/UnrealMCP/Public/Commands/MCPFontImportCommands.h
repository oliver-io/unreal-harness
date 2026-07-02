#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for external font import driven from MCP.
 *
 * asset_import_font is the headless equivalent of dragging a .ttf/.otf into the Content
 * Browser AND wiring it up for use: it runs the engine's UFontFileImportFactory through
 * UAssetImportTask to create a UFontFace (the raw FreeType payload), then builds a
 * **UFont** (FontCacheType=Runtime, the kind Slate/UMG/FSlateFontInfo consume) whose
 * DefaultTypeface references that face under a "Regular" entry. So one call yields a
 * ready-to-reference Font asset, not just the bare face the editor import leaves you with.
 *
 * This is the font analogue of FMCPSoundImportCommands / FMCPTextureCommands — same
 * UAssetImportTask machinery, different factory + a post-import asset-construction step.
 *
 * Source-format note: .ttf and .otf are both handled by UFontFileImportFactory.
 *
 * Natural home for future font-asset tools (asset_reimport_font, multi-typeface composites…).
 */
class FMCPFontImportCommands
{
public:
	FMCPFontImportCommands();

	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	/**
	 * Import one or more external font files as runtime UFont assets (+ their UFontFace).
	 *
	 * Required params:
	 *   destination_folder (string) — /Game/... package path the assets land in
	 *   fonts              (array)  — non-empty; each entry { path, name? }
	 *
	 * Per-entry fields:
	 *   path     (string) — absolute filesystem path to the source .ttf/.otf
	 *   name     (string) — destination Font asset name; defaults to the basename. The
	 *                       backing face is created as "<name>_Face" alongside it.
	 *
	 * Optional params:
	 *   force_overwrite (bool=true) — replace existing assets at the destination
	 *
	 * Response on success:
	 *   { count, imported: [ { asset_path, face_path, typeface } ], failed: [ { path, reason } ] }
	 */
	TSharedPtr<FJsonObject> HandleImportFont(const TSharedPtr<FJsonObject>& Params);
};
