/**
 * Domain: level — editor level inspection, World Settings overrides, and
 * level (UWorld / .umap) persistence (new / save / save_as / load).
 *
 * The inspect / gamemode tools forward 1:1 to a wire command of the same name.
 * The four persistence tools (GAP-006) are mutators: blocked during PIE and
 * refuse dry_run (see IsBlockedDuringPie / IsBlockedFromDryRun in
 * MCPCommonUtils.cpp, mirrored in src/server/src/bridge/gates.ts).
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const levelInspect = bridgeTool({
  name: "level_inspect",
  domain: "level",
  description:
    "Inspect the current editor level — name, path, world_type, persistent_level, " +
    "world_settings, and full sublevel topology (loaded + unloaded). Read-only.",
  input: z.object({}),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const levelSetGamemodeOverride = bridgeTool({
  name: "level_set_gamemode_override",
  domain: "level",
  description:
    "Set a level's World Settings → GameModeOverride (AWorldSettings::DefaultGameMode). " +
    "Mutates the in-memory AWorldSettings, marks dirty, and saves the .umap. " +
    'Pass "" or "None" as gamemode_class to clear the override.',
  input: z.object({
    level_path: z
      .string()
      .describe(
        'Full /Game/... path to the level/world asset (e.g. "/Game/Levels/Arena" or "/Game/Levels/Arena.Arena").',
      ),
    gamemode_class: z
      .string()
      .describe(
        'GameMode to bind. BP path ("/Game/.../BP_X" — auto-appends "_C") | native path ' +
          '("/Script/Module.AClassName") | "" or "None" to clear.',
      ),
  }),
  annotations: { idempotentHint: true },
});

const levelNew = bridgeTool({
  name: "level_new",
  domain: "level",
  description:
    "Create a new blank level, replacing the current editor world (the outgoing " +
    "world is NOT auto-saved — call level_save first to keep it). Optionally seed " +
    "from a template level. Returns the new map_name + package_path (a transient " +
    "/Temp/ world until you level_save_as). Blocked during PIE; dry_run unsupported.",
  input: z.object({
    template: z
      .string()
      .optional()
      .describe(
        'Optional /Game/... path to a template level to copy. Omit or pass "" for a blank map.',
      ),
  }),
});

const levelSave = bridgeTool({
  name: "level_save",
  domain: "level",
  description:
    "Save the CURRENT editor world to its existing on-disk package. Errors with " +
    "invalid_path on a transient /Temp/ untitled level (use level_save_as to give " +
    "it a path first). Returns saved map_name + package_path. Blocked during PIE; " +
    "dry_run unsupported.",
  input: z.object({}),
  annotations: { idempotentHint: true },
});

const levelSaveAs = bridgeTool({
  name: "level_save_as",
  domain: "level",
  description:
    "Save the CURRENT editor world to a new /Game/... content path (the headless " +
    "File > Save Current As). Creates the package on disk and makes it the active " +
    "level. Returns saved map_name + package_path. Blocked during PIE; dry_run " +
    "unsupported.",
  input: z.object({
    package_path: z
      .string()
      .describe(
        'Destination /Game/... package path, no extension (e.g. "/Game/Maps/L_MyLevel").',
      ),
  }),
});

const levelLoad = bridgeTool({
  name: "level_load",
  domain: "level",
  description:
    "Open an existing level by /Game/... package path, replacing the current " +
    "editor world (does not prompt to save the outgoing world). Returns loaded " +
    "map_name + package_path. Blocked during PIE; dry_run unsupported.",
  input: z.object({
    package_path: z
      .string()
      .describe(
        'Full /Game/... package path to an existing level (e.g. "/Game/Maps/L_MyLevel").',
      ),
  }),
  annotations: { idempotentHint: true },
});

export const levelTools: ToolDef[] = [
  levelInspect,
  levelSetGamemodeOverride,
  levelNew,
  levelSave,
  levelSaveAs,
  levelLoad,
];
