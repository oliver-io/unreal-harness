/**
 * Maps a UE asset class (as reported by `asset_list`) to the canonical MCP read
 * tool that returns its structure, plus the param key that carries the asset path.
 *
 * This is the ONLY place that knows tool names. To support a new asset type, add
 * a row here — no other file changes. Names are this harness's canonical surface
 * (statetree_read, bp_read, …); a different MCP server may use different names.
 */

export interface Reader {
  tool: string;
  /** Param key the read tool expects the asset path under. */
  pathParam: string;
  /** Extra static args merged into the call (e.g. include_* toggles). */
  args?: Record<string, unknown>;
}

export const READERS: Record<string, Reader> = {
  Blueprint: { tool: "bp_read", pathParam: "blueprint_path" },
  StateTree: { tool: "statetree_read", pathParam: "asset_path" },
  Material: { tool: "material_read", pathParam: "material_path" },
  MaterialInstanceConstant: { tool: "material_read_instance", pathParam: "instance_path" },
  NiagaraSystem: { tool: "niagara_system_read", pathParam: "system_path" },
  DataTable: { tool: "asset_datatable_read", pathParam: "table_path" },
};

/** Classes that derive from UDataAsset are read generically via reflection. */
const DATAASSET_FALLBACK: Reader = { tool: "asset_dataasset_read", pathParam: "asset_path" };

export function readerFor(cls: string, opts: { dataAssets?: boolean } = {}): Reader | null {
  if (READERS[cls]) return READERS[cls];
  // Heuristic: anything ending in "DataAsset" (or when explicitly allowed) can be
  // reflection-dumped. Conservative by default — only when --data-assets is passed.
  if (opts.dataAssets && /DataAsset$/.test(cls)) return DATAASSET_FALLBACK;
  return null;
}
