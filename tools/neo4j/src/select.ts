/**
 * Asset selection — always derived from the LIVE project via `asset_list`, then
 * narrowed client-side. Nothing is hardcoded; the caller scopes with flags.
 */

import type { Mcp } from "./mcp.ts";

export interface Asset {
  path: string;
  class: string;
  name?: string;
}

export interface Selector {
  dir: string; // directory_path for asset_list
  cls: string; // class_filter (server-side), e.g. "Blueprint"
  recursive: boolean;
  filter?: string; // client-side substring on name/path (case-insensitive)
  paths?: string[]; // client-side whitelist of exact paths (suffix-insensitive)
}

const stem = (p: string): string => p.split(".")[0]!; // /Game/X.X -> /Game/X

export async function selectAssets(mcp: Mcp, sel: Selector): Promise<Asset[]> {
  const res = await mcp.call("asset_list", {
    directory_path: sel.dir,
    recursive: sel.recursive,
    class_filter: sel.cls,
  });
  let assets: Asset[] = (res?.assets ?? []).map((a: any) => ({
    path: a.path,
    class: a.class,
    name: a.name,
  }));

  if (sel.filter) {
    const f = sel.filter.toLowerCase();
    assets = assets.filter(
      (a) => a.path.toLowerCase().includes(f) || (a.name ?? "").toLowerCase().includes(f),
    );
  }
  if (sel.paths && sel.paths.length) {
    const wanted = new Set(sel.paths.map(stem));
    assets = assets.filter((a) => wanted.has(stem(a.path)));
  }
  return assets;
}
