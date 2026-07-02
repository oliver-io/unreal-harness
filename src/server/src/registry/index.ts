/**
 * The tool registry — register, look up, search, describe, dispatch.
 *
 * This is the backbone for all three disclosure tiers:
 *   - Tier 1 (client tool-search): `list()` / `toMcpTools()` expose names +
 *     descriptions + JSON Schema for clients that defer schemas natively.
 *   - Tier 2 (server meta-tools): `search()` / `describe()` / `dispatch()` back
 *     `unreal_search_tools` / `unreal_describe_tool` / `unreal_call`.
 *   - Tier 3 (code-execution): the same defs generate the typed `unreal.*` API.
 */

import { zodToJsonSchema } from "zod-to-json-schema";
import type { ToolDef, ToolSummary } from "./types.ts";

export class ToolRegistry {
  private readonly tools = new Map<string, ToolDef>();

  /** Register a tool. Throws on duplicate name (a programming error). */
  register(def: ToolDef): void {
    if (this.tools.has(def.name)) {
      throw new Error(`duplicate tool registration: ${def.name}`);
    }
    this.tools.set(def.name, def);
  }

  registerAll(defs: readonly ToolDef[]): void {
    for (const d of defs) this.register(d);
  }

  get(name: string): ToolDef | undefined {
    return this.tools.get(name);
  }

  has(name: string): boolean {
    return this.tools.has(name);
  }

  size(): number {
    return this.tools.size;
  }

  all(): ToolDef[] {
    return [...this.tools.values()];
  }

  domains(): { domain: string; count: number }[] {
    const counts = new Map<string, number>();
    for (const t of this.tools.values()) {
      counts.set(t.domain, (counts.get(t.domain) ?? 0) + 1);
    }
    return [...counts.entries()]
      .map(([domain, count]) => ({ domain, count }))
      .sort((a, b) => a.domain.localeCompare(b.domain));
  }

  /** JSON Schema for one tool's input (lazily computed, cached per def). */
  inputJsonSchema(name: string): Record<string, unknown> | undefined {
    const def = this.tools.get(name);
    if (!def) return undefined;
    return zodToJsonSchema(def.input, { target: "jsonSchema7" }) as Record<
      string,
      unknown
    >;
  }

  summary(name: string): ToolSummary | undefined {
    const def = this.tools.get(name);
    if (!def) return undefined;
    return {
      name: def.name,
      domain: def.domain,
      description: def.description,
      annotations: def.annotations,
    };
  }

  /**
   * Rank tools against a free-text query by name + description match. Cheap
   * dependency-free scoring (no embeddings) to stay sleek; good enough for the
   * meta-tool search path. Optional `domain` filter.
   */
  search(
    query: string,
    opts?: { domain?: string; limit?: number },
  ): ToolSummary[] {
    const terms = query.toLowerCase().split(/\s+/).filter(Boolean);
    const limit = opts?.limit ?? 20;
    const scored: { s: ToolSummary; score: number }[] = [];

    for (const def of this.tools.values()) {
      if (opts?.domain && def.domain !== opts.domain) continue;
      const name = def.name.toLowerCase();
      const desc = def.description.toLowerCase();
      let score = 0;
      for (const term of terms) {
        if (name === term) score += 100;
        else if (name.includes(term)) score += 10;
        if (def.domain.toLowerCase() === term) score += 8;
        if (desc.includes(term)) score += 3;
      }
      // No query terms → list everything (filtered) at a flat score.
      if (terms.length === 0 || score > 0) {
        scored.push({ s: this.summary(def.name)!, score });
      }
    }

    return scored
      .sort((a, b) => b.score - a.score || a.s.name.localeCompare(b.s.name))
      .slice(0, limit)
      .map((x) => x.s);
  }
}
