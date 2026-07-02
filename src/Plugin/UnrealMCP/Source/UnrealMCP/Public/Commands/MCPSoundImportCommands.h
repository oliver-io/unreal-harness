#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for external audio import driven from MCP.
 *
 * asset_import_audio is the headless equivalent of dragging a .wav/.mp3/.ogg/.flac/
 * .aif(f)/.opus into the Content Browser: it runs the engine's USoundFactory through
 * UAssetImportTask + FAssetToolsModule::ImportAssetTasks, producing a USoundWave under
 * /Game/. This is the audio analogue of FMCPTextureCommands::HandleImportTextures —
 * same UAssetImportTask machinery, different factory (USoundFactory) and a single
 * post-import staged setting (`looping` → USoundWave::bLooping, applied with the
 * PreEditChange → set → PostEditChange → save contract before the package is written).
 *
 * Source-format note: .mp3/.ogg/.flac/.aif/.opus import is gated on WITH_SNDFILE_IO
 * (USoundFactory::Formats), which is enabled in editor builds — .wav always works.
 *
 * This file is the natural home for future audio-asset import tools
 * (asset_reimport_audio, SoundCue authoring, attenuation defaults, …).
 */
class FMCPSoundImportCommands
{
public:
	FMCPSoundImportCommands();

	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	/**
	 * Import one or more external audio files as USoundWave assets.
	 *
	 * Required params:
	 *   destination_folder (string) — /Game/... package path the assets land in
	 *   sounds             (array)  — non-empty; each entry { path, name?, looping? }
	 *
	 * Per-entry fields:
	 *   path     (string)     — absolute filesystem path to the source audio file
	 *   name     (string)     — destination asset name; defaults to the basename
	 *   looping  (bool=false) — set USoundWave::bLooping (so a UAudioComponent loops it)
	 *
	 * Optional params:
	 *   force_overwrite (bool=true) — replace an existing asset at the destination
	 *
	 * Response on success:
	 *   { count, imported: [ { asset_path, looping } ], failed: [ { path, reason } ] }
	 */
	TSharedPtr<FJsonObject> HandleImportAudio(const TSharedPtr<FJsonObject>& Params);
};
