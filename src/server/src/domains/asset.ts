/**
 * Domain: asset — content-browser asset CRUD, queries, and pipeline imports.
 *
 * Port of the `asset_*` tools in `src/MCP/server.py`. Lifecycle mutators
 * (rename/move/duplicate/delete/save/fixup/open) route through
 * `helpers/asset_manager.py`; the wire command equals the tool name in every
 * case. Read tools (list/references/datatable_read/dataasset_read) are PIE-safe;
 * the mutators are refused while a PIE session is active.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";
import { dryRun } from "./_schemas.ts";

const assetList = bridgeTool({
  name: "asset_list",
  domain: "asset",
  description:
    "List assets under a directory. Returns count + assets[] (path, class, name). " +
    "class_filter narrows by type (Blueprint, Material, StaticMesh, …).",
  input: z.object({
    directory_path: z.string().default("/Game/"),
    recursive: z.boolean().default(true),
    class_filter: z.string().default(""),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  // class_filter omitted when empty (Python maps "" → None → key absent).
  params: (a) => {
    const p: Record<string, unknown> = {
      directory_path: a.directory_path,
      recursive: a.recursive,
    };
    if (a.class_filter) p.class_filter = a.class_filter;
    return p;
  },
});

const assetReferences = bridgeTool({
  name: "asset_references",
  domain: "asset",
  description:
    "Direction-aware lookup over UE's asset reference graph (GetDependencies " +
    "outbound / GetReferencers inbound). Read-only, paginated. direction is one of " +
    "'outbound' (what this asset uses) | 'inbound' (what uses it).",
  input: z.object({
    asset_path: z
      .string()
      .describe(
        'Package name ("/Game/Foo/Bar") or object path ("/Game/Foo/Bar.Bar"); suffix stripped.',
      ),
    direction: z
      .string()
      .describe("'outbound' (what does this asset use?) or 'inbound' (what uses it?). Closed set."),
    depth: z
      .number()
      .int()
      .default(1)
      .describe("1..10. 1 = direct (one-hop) only; higher walks the graph transitively."),
    cursor: z.number().int().default(0).describe("Pagination offset; pass next_cursor from a prior response."),
    limit: z.number().int().default(200).describe("Page size, 1..1000. Default 200."),
    include_hard: z.boolean().default(true).describe("Include hard references. At least one kind must stay true."),
    include_soft: z.boolean().default(true).describe("Include soft references."),
    include_searchable_name: z.boolean().default(true).describe("Include searchable-name references."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const assetDatatableRead = bridgeTool({
  name: "asset_datatable_read",
  domain: "asset",
  description:
    "Read the full contents of a DataTable asset: row struct, columns, and all rows. " +
    "Returns table_name, table_path, row_struct, columns, rows, row_count.",
  input: z.object({
    table_path: z
      .string()
      .describe('Full path to the DataTable asset (e.g. "/Game/Data/DT_Skills.DT_Skills").'),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const assetDataassetRead = bridgeTool({
  name: "asset_dataasset_read",
  domain: "asset",
  description:
    "JSON-dump every edit-exposed property on a UDataAsset instance (reflection-walked, so new " +
    "UPROPERTYs show up automatically). Returns asset_path, class (FQN), properties.",
  input: z.object({
    asset_path: z.string().describe("Full /Game/... path to the UDataAsset instance."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

const assetDataassetCreate = bridgeTool({
  name: "asset_dataasset_create",
  domain: "asset",
  description:
    "Create an empty UDataAsset of the given class at a /Game-relative path (mirrors blueprint " +
    "path/class resolution). Arrays start empty — populate via asset_dataasset_set_property.",
  input: z.object({
    name: z
      .string()
      .min(1)
      .describe('Bare name ("DA_X" → /Game/DataAssets/DA_X) or full /Game/... path. Folders auto-created.'),
    asset_class: z
      .string()
      .min(1)
      .describe('Short ("WarmupProfile"), UE-prefixed ("UWarmupProfile"), or FQN ("/Script/MyGame.WarmupProfile"). Must derive from UDataAsset.'),
    force_overwrite: z.boolean().default(false).describe("Replace any existing asset at the path. Default false errors if occupied."),
  }),
  annotations: { blockedDuringPie: true },
  // Wire param key is `class` (matches Python).
  params: (a) => ({
    name: a.name,
    class: a.asset_class,
    force_overwrite: a.force_overwrite,
  }),
});

const assetDataassetSetProperty = bridgeTool({
  name: "asset_dataasset_set_property",
  domain: "asset",
  description:
    "Write an edit-exposed property on a UDataAsset CDO (any FProperty type). For array " +
    "properties, action selects set | append | clear | remove_at. Auto-saves on success.",
  input: z.object({
    asset_path: z.string().describe("Full /Game/... path to the UDataAsset instance."),
    property: z.string().describe("UPROPERTY name (case-sensitive, matches the C++ field)."),
    value: z
      .unknown()
      .optional()
      .describe("JSON-compatible value matching the property type. For soft refs, pass the asset path string. Ignored for clear/remove_at."),
    action: z
      .string()
      .default("set")
      .describe('One of "set", "append", "clear", "remove_at" (arrays); "set" for scalars.'),
    index: z.number().int().default(-1).describe('Required when action == "remove_at"; ignored otherwise.'),
  }),
  annotations: { blockedDuringPie: true },
  // value omitted when unset; index omitted when negative (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = {
      asset_path: a.asset_path,
      property: a.property,
      action: a.action,
    };
    if (a.value != null) p.value = a.value;
    if (a.index >= 0) p.index = a.index;
    return p;
  },
});

/** Per-image import spec for asset_textures_import. */
const TextureImage = z.object({
  path: z
    .string()
    .describe("Absolute filesystem path to the source image (Windows or POSIX separators)."),
  name: z.string().optional().describe("Destination asset name; defaults to the basename of path."),
  settings: z
    .object({
      sRGB: z.boolean().optional().describe("True for albedo/preview, False for data textures (normal, ORM, height…)."),
      compression: z
        .string()
        .optional()
        .describe('One of "Default", "NormalMap", "Masks", "Grayscale", "Displacementmap", "HDR", "EditorIcon", "Alpha".'),
      lod_group: z
        .string()
        .optional()
        .describe(
          'Texture LOD group: one of "World" (default), "UI", "Effects", "Skybox", "Character", "Weapon", ' +
            '"Vehicle", "Pixels2D". Use "UI" for HUD/widget art — UI-group textures are never streamed, so the ' +
            "full mip is always resident (a World-group HUD texture renders a blurry low mip until streaming catches up)."
        ),
      mip_gen: z
        .string()
        .optional()
        .describe(
          'Mip generation: one of "FromTextureGroup" (default), "NoMipmaps", "SimpleAverage", "LeaveExistingMips". ' +
            '"NoMipmaps" suits UI art drawn near 1:1 — no lower mip to blur into, and mips waste memory there.'
        ),
      composite_texture: z
        .string()
        .optional()
        .describe("/Game/... path to a normal map whose mip variance is baked into this texture's roughness (Toksvig). Source must appear earlier in images[]."),
      composite_mode: z
        .string()
        .optional()
        .describe('One of "NormalRoughnessToRed", "NormalRoughnessToGreen", "NormalRoughnessToBlue", "NormalRoughnessToAlpha".'),
    })
    .optional()
    .describe("Per-image UE settings."),
});

const assetTexturesImport = bridgeTool({
  name: "asset_textures_import",
  domain: "asset",
  description:
    "Import one or more image files as UTexture2D assets into a content folder (runs UTextureFactory, " +
    "then applies per-image sRGB/compression/lod_group/mip_gen and saves). Returns count, imported[], failed[]. " +
    'For HUD/widget art pass settings.lod_group:"UI" + mip_gen:"NoMipmaps" or it streams in blurry on first use.',
  input: z.object({
    destination_folder: z
      .string()
      .describe('/Game/... package path the textures land in (e.g. "/Game/Generated/red_clay"). Created if missing.'),
    images: z.array(TextureImage).describe("List of per-image specs (path required; name + settings optional)."),
    force_overwrite: z
      .boolean()
      .default(true)
      .describe("If true (default), replace existing assets. False skips the file (recorded under failed)."),
  }),
  annotations: { blockedDuringPie: true },
});

const assetImportMesh = bridgeTool({
  name: "asset_import_mesh",
  domain: "asset",
  description:
    "Import an external mesh file (.fbx, and where the same engine path supports it .obj/.gltf) from a " +
    "filesystem path into the project as a UStaticMesh (or USkeletalMesh) — the headless equivalent of " +
    "dragging an FBX into the Content Browser (runs UFbxFactory via UAssetImportTask, then saves). " +
    "Returns count, imported[] ({source, asset_path, class}), created_materials[], created_textures[], failed[]. " +
    "NOTE: FBX exported from V-Ray/Corona/other DCC renderers usually imports with the engine's DEFAULT " +
    "materials (source shaders don't map onto UE's model) — expect to rebuild materials afterwards " +
    "(material_create + mesh_set_static_mesh_material). Skeletal import needs the FBX to contain a rig, " +
    "or pass an existing `skeleton` to bind to.",
  input: z.object({
    source_path: z
      .string()
      .describe("Absolute filesystem path to the source mesh file (.fbx/.obj/.gltf; Windows or POSIX separators)."),
    destination_folder: z
      .string()
      .describe('/Game/... package path the asset lands in (e.g. "/Game/Vehicles/Bike"). Created if missing.'),
    name: z.string().optional().describe("Destination asset name; defaults to the basename of source_path."),
    import_as_skeletal: z
      .boolean()
      .default(false)
      .describe("Import as USkeletalMesh instead of UStaticMesh. Requires a rig in the FBX or a `skeleton`."),
    skeleton: z
      .string()
      .optional()
      .describe("/Game/... path to an existing USkeleton to bind a skeletal import to. Omit to create a new one."),
    import_materials: z.boolean().default(true).describe("Import embedded materials from the source file."),
    import_textures: z.boolean().default(true).describe("Import embedded textures from the source file."),
    combine_meshes: z
      .boolean()
      .default(false)
      .describe("Merge all mesh nodes in the file into one asset (static-mesh import only)."),
    force_overwrite: z
      .boolean()
      .default(true)
      .describe("If true (default), replace an existing asset at the destination; otherwise the import fails."),
  }),
  annotations: { blockedDuringPie: true },
});

/** Per-sound import spec for asset_import_audio. */
const AudioSound = z.object({
  path: z
    .string()
    .describe("Absolute filesystem path to the source audio file (.wav/.mp3/.ogg/.flac/.aif(f)/.opus; Windows or POSIX separators)."),
  name: z.string().optional().describe("Destination asset name; defaults to the basename of path."),
  looping: z
    .boolean()
    .default(false)
    .describe("Set USoundWave::bLooping so a UAudioComponent plays it as a seamless loop (engine/ambient/music). Leave false for one-shots."),
});

const assetImportAudio = bridgeTool({
  name: "asset_import_audio",
  domain: "asset",
  description:
    "Import one or more external audio files as USoundWave assets into a content folder — the headless " +
    "equivalent of dragging a .wav/.mp3/.ogg/.flac/.aif(f)/.opus into the Content Browser (runs USoundFactory " +
    "via UAssetImportTask, applies the per-sound `looping` flag, then saves). Returns count, imported[] " +
    "({asset_path, looping}), failed[]. NOTE: non-.wav formats import only where the engine was built with " +
    "WITH_SNDFILE_IO (on in editor builds); .wav always works. Set looping:true for engine/ambient/music beds " +
    "that a UAudioComponent should loop — one-shots (impacts, stingers) stay false.",
  input: z.object({
    destination_folder: z
      .string()
      .describe('/Game/... package path the sounds land in (e.g. "/Game/MyGame/Audio"). Created if missing.'),
    sounds: z.array(AudioSound).describe("List of per-sound specs (path required; name + looping optional)."),
    force_overwrite: z
      .boolean()
      .default(true)
      .describe("If true (default), replace an existing asset at the destination; otherwise the file is recorded under failed."),
  }),
  annotations: { blockedDuringPie: true },
});

/** Per-font import spec for asset_import_font. */
const FontFile = z.object({
  path: z
    .string()
    .describe("Absolute filesystem path to the source font file (.ttf or .otf; Windows or POSIX separators)."),
  name: z
    .string()
    .optional()
    .describe('Destination Font asset name; defaults to the basename of path. The backing face is created as "<name>_Face" alongside it.'),
});

const assetImportFont = bridgeTool({
  name: "asset_import_font",
  domain: "asset",
  description:
    "Import one or more external font files (.ttf/.otf) as ready-to-use runtime UFont assets — the headless " +
    "equivalent of dragging a font into the Content Browser AND wiring it up. For each file it runs " +
    "UFontFileImportFactory (→ a UFontFace) then builds a UFont (FontCacheType=Runtime) whose default typeface " +
    'references that face under a "Regular" entry, so the result is directly usable by FSlateFontInfo / UMG ' +
    "TextBlocks / Slate (unlike the bare UFontFace the editor import alone leaves you with). Returns count, " +
    "imported[] ({asset_path, face_path, typeface}), failed[] ({path, reason}). Reference the returned " +
    "asset_path as the FontObject; the typeface name is \"Regular\".",
  input: z.object({
    destination_folder: z
      .string()
      .describe('/Game/... package path the fonts land in (e.g. "/Game/MyGame/UI/Fonts"). Created if missing.'),
    fonts: z.array(FontFile).describe("List of per-font specs (path required; name optional)."),
    force_overwrite: z
      .boolean()
      .default(true)
      .describe("If true (default), replace existing assets at the destination; otherwise the file is recorded under failed."),
  }),
  annotations: { blockedDuringPie: true },
});

const assetRename = bridgeTool({
  name: "asset_rename",
  domain: "asset",
  description:
    "Rename an asset in-place (same folder, new leaf name). Leaves a redirector at the old path. " +
    "dry_run returns result.diff {renamed: [{from, to}]} without committing.",
  input: z.object({
    source_path: z.string().describe('Full package path of the asset (e.g. "/Game/Blueprints/BP_Horse").'),
    new_name: z.string().describe('New asset name without path (e.g. "BP_WarHorse").'),
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
});

const assetMove = bridgeTool({
  name: "asset_move",
  domain: "asset",
  description:
    "Move an asset to a different folder, optionally renaming it. Leaves a redirector at the old path. " +
    "dry_run returns result.diff {moved: [{from, to}]} without committing.",
  input: z.object({
    source_path: z.string().describe('Full package path of the asset (e.g. "/Game/Blueprints/BP_Horse").'),
    destination_folder: z.string().describe('Target folder path (e.g. "/Game/Vehicles/Mounts").'),
    new_name: z.string().default("").describe("Optional new name — keeps the original name if empty."),
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
  // new_name omitted when empty (Python passes None → key absent).
  params: (a) => {
    const p: Record<string, unknown> = {
      source_path: a.source_path,
      destination_folder: a.destination_folder,
      dry_run: a.dry_run,
    };
    if (a.new_name) p.new_name = a.new_name;
    return p;
  },
});

const assetDuplicate = bridgeTool({
  name: "asset_duplicate",
  domain: "asset",
  description:
    "Duplicate an asset to a new path; the copy is independent. dry_run returns " +
    "result.diff {created: [{path, source_path}]} without committing.",
  input: z.object({
    source_path: z.string().describe('Full package path of the source (e.g. "/Game/Blueprints/BP_Horse").'),
    destination_path: z.string().describe('Full package path for the copy (e.g. "/Game/Blueprints/BP_Horse_V2").'),
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
});

const assetSave = bridgeTool({
  name: "asset_save",
  domain: "asset",
  description: "Save assets to disk. Empty asset_paths saves ALL dirty (modified) assets.",
  input: z.object({
    asset_paths: z
      .array(z.string())
      .default([])
      .describe("Package paths to save. Empty list = save all dirty assets."),
  }),
  annotations: { blockedDuringPie: true },
});

const assetFixupRedirectors = bridgeTool({
  name: "asset_fixup_redirectors",
  domain: "asset",
  description:
    "Consolidate redirectors so all references point directly to new asset locations (IAssetTools::" +
    "FixupReferencers). Run after batch renames/moves. dry_run returns result.diff {deleted: [{path}]}.",
  input: z.object({
    directory_path: z.string().default("/Game/").describe("Folder to scan for redirectors (recursive)."),
    dry_run: dryRun,
  }),
  annotations: { blockedDuringPie: true },
});

const assetDelete = bridgeTool({
  name: "asset_delete",
  domain: "asset",
  description:
    "Delete an asset. Refuses if other assets reference it unless force=true (references break). " +
    "dry_run returns result.diff {deleted: [{path}]} (+ referencers_orphaned[] when force=true).",
  input: z.object({
    asset_path: z.string().describe('Full package path of the asset (e.g. "/Game/Blueprints/BP_Test").'),
    force: z.boolean().default(false).describe("Delete even if other assets reference this one."),
    dry_run: dryRun,
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
});

const assetBakeDynamicToStaticMesh = bridgeTool({
  name: "asset_bake_dynamic_to_static_mesh",
  domain: "asset",
  description:
    "Bake a UDynamicMeshComponent's current mesh into a saved UStaticMesh asset (headless Modeling Mode " +
    "→ Bake → Static Mesh). Promotes procedural geometry into a referenceable .uasset.",
  input: z.object({
    actor_name: z.string().describe("Label or name of the actor in the loaded editor level."),
    target_asset_path: z.string().describe('/Game/... package path for the new static mesh (e.g. "/Game/Objects/SM_Barrel").'),
    component_name: z
      .string()
      .default("")
      .describe("Optional UDynamicMeshComponent name. Empty auto-picks a singleton; errors if >1 DMC."),
    force_overwrite: z.boolean().default(false).describe("Replace an existing asset at target_asset_path; otherwise errors if taken."),
    material_paths: z
      .array(z.string())
      .default([])
      .describe("Optional per-slot material overrides (asset paths). Index N overrides slot N; empty entries keep source."),
    collision_trace_flag: z
      .string()
      .default("simple_as_complex")
      .describe('One of "default", "simple_as_complex", "complex_as_simple", "use_complex_collision".'),
    recompute_normals: z.boolean().default(false).describe("Regenerate normals during the bake (usually source normals are correct)."),
    recompute_tangents: z.boolean().default(true).describe("Regenerate tangents during the bake."),
    enable_nanite: z.boolean().default(false).describe("Enable Nanite on the baked static mesh."),
    replace_source_actor: z
      .boolean()
      .default(false)
      .describe("After bake, destroy the source actor and spawn an AStaticMeshActor at its transform pointing at the new asset."),
  }),
  annotations: { blockedDuringPie: true },
});

const assetOpen = bridgeTool({
  name: "asset_open",
  domain: "asset",
  description:
    "Open an asset in its registered editor (like double-clicking it in the Content Browser). Works for " +
    "Blueprints, Materials, Data Tables, Textures, and any asset with a registered editor.",
  input: z.object({
    asset_path: z.string().describe('Full package path of the asset (e.g. "/Game/Materials/M_Landscape").'),
  }),
  annotations: { blockedDuringPie: true },
});

export const assetTools: ToolDef[] = [
  assetList,
  assetReferences,
  assetDatatableRead,
  assetDataassetRead,
  assetDataassetCreate,
  assetDataassetSetProperty,
  assetTexturesImport,
  assetImportMesh,
  assetImportAudio,
  assetImportFont,
  assetRename,
  assetMove,
  assetDuplicate,
  assetSave,
  assetFixupRedirectors,
  assetDelete,
  assetBakeDynamicToStaticMesh,
  assetOpen,
];
