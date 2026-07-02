#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for texture asset operations driven from MCP.
 *
 * The first command, import_textures, is the headless equivalent of dragging
 * one or more PNG/JPG/EXR files into the Content Browser: it runs the engine's
 * UTextureFactory through UAssetImportTask, then applies per-channel UE
 * settings (sRGB flag, CompressionSettings) that callers cannot otherwise
 * specify at import time. This is the entry point the texture skill uses to
 * land procedurally-generated PBR sets into /Game/Generated/<slug>/.
 *
 * The settings post-pass matters for PBR — basecolor needs sRGB+TC_Default,
 * normal needs TC_Normalmap (else compressed as colour and breaks the surface
 * frame), ORM-packed needs TC_Masks (linear, no sRGB curve), and height needs
 * TC_Grayscale or TC_Displacementmap. Without setting these, every imported
 * map gets the engine's "guess from filename" defaults which are wrong as
 * often as they are right.
 *
 * This file is the natural home for future texture-asset tools
 * (set_texture_compression, batch_resize_textures, …).
 */
class FMCPTextureCommands
{
public:
	FMCPTextureCommands();

	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	/**
	 * Import one or more image files as UTexture2D assets.
	 *
	 * Required params:
	 *   destination_folder (string)  — /Game/... package path for the imported textures
	 *   images (array of object)     — each entry: { path, name?, settings? }
	 *
	 * Per-image fields:
	 *   path     (string, required) — filesystem path to the source image (PNG/JPG/EXR/TGA)
	 *   name     (string, optional) — destination asset name; defaults to basename of path
	 *   settings (object, optional) — { sRGB (bool), compression (string) }
	 *     compression values:
	 *       "Default"           → TC_Default          (basecolor, generic)
	 *       "NormalMap"         → TC_Normalmap        (tangent-space normals)
	 *       "Masks"             → TC_Masks            (ORM, packed linear data)
	 *       "Grayscale"         → TC_Grayscale        (single-channel linear)
	 *       "Displacementmap"   → TC_Displacementmap  (16-bit height)
	 *       "HDR"               → TC_HDR              (linear RGB float)
	 *       "EditorIcon"        → TC_EditorIcon       (8-bit, no compression)
	 *       "Alpha"             → TC_Alpha            (alpha/opacity only)
	 *
	 * Optional params:
	 *   force_overwrite (bool=true) — replace existing assets at the destination
	 *
	 * Response on success:
	 *   { count, imported: [ { asset_path, size_x, size_y, sRGB, compression } ],
	 *     failed: [ { path, reason } ] }
	 */
	TSharedPtr<FJsonObject> HandleImportTextures(const TSharedPtr<FJsonObject>& Params);
};
