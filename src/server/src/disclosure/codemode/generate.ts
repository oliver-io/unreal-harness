/**
 * Generate the typed `unreal.*` API surface that code-mode snippets call.
 *
 * Kept deliberately compact: one line per tool (`unreal.<name>(params)` + the
 * one-line description), optionally scoped to a domain. Exact parameter schemas
 * are fetched on demand via `catalog_describe` — so the API listing itself stays
 * cheap (the whole point of progressive disclosure). This composes the tiers:
 * code-mode for control flow, catalog_describe for schemas.
 */

import type { ToolRegistry } from "../../registry/index.ts";

export function generateApi(registry: ToolRegistry, domain?: string): string {
  const byDomain = new Map<string, { name: string; description: string }[]>();
  for (const def of registry.all()) {
    if (domain && def.domain !== domain) continue;
    if (def.domain === "catalog" || def.domain === "code") continue; // not callable from snippets
    if (!byDomain.has(def.domain)) byDomain.set(def.domain, []);
    byDomain.get(def.domain)!.push({ name: def.name, description: def.description });
  }

  const lines: string[] = [
    "// Unreal code-mode API. Every call returns Promise<{status,result,error}>.",
    "// Exact params: call catalog_describe('<tool>'). Filter/aggregate in code;",
    "// only console.log(...) and the snippet's return value reach the model.",
    "declare const unreal: {",
  ];
  for (const d of [...byDomain.keys()].sort()) {
    lines.push(`  // ── ${d} ──`);
    for (const t of byDomain.get(d)!.sort((a, b) => a.name.localeCompare(b.name))) {
      lines.push(`  ${t.name}(params?: object): Promise<any>; // ${t.description}`);
    }
  }
  lines.push("};");
  return lines.join("\n");
}
