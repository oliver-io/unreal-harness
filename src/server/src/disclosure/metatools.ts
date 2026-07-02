/**
 * Tier 2 — server-side progressive disclosure via `catalog_*` meta-tools.
 *
 * For clients without native tool-search, these four tools collapse the 233-tool
 * schema tax to a handful of always-on tools. The agent discovers
 * (`catalog_domains` / `catalog_search`), inspects one schema on demand
 * (`catalog_describe`), then invokes (`catalog_call`) — schemas enter context
 * only when actually needed.
 *
 * They are ordinary {@link ToolDef}s over the same registry, so `catalog_call`
 * runs a tool through the identical validate→handler→envelope path as a direct
 * call.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import type { ToolRegistry } from "../registry/index.ts";
import { applyAliases } from "../registry/aliases.ts";
import { defineTool } from "../domains/_shared.ts";
import { envelopeError } from "../bridge/envelope.ts";
import { isPieBlocked, isDryRunUnsupported } from "../bridge/gates.ts";

/** Domains that are discovery plumbing, hidden from catalog_* results. */
const META_DOMAINS = new Set(["catalog", "code"]);

export function metaTools(registry: ToolRegistry): ToolDef[] {
  const catalogDomains = defineTool({
    name: "catalog_domains",
    domain: "catalog",
    description:
      "List the tool domains and how many tools each holds. Start here to see " +
      "the shape of the Unreal toolset, then catalog_search within a domain.",
    input: z.object({}),
    annotations: { readOnlyHint: true, idempotentHint: true },
    handler: () => {
      const domains = registry.domains().filter((d) => !META_DOMAINS.has(d.domain));
      const total = domains.reduce((n, d) => n + d.count, 0);
      return { status: "success", result: { total, domains } };
    },
  });

  const catalogSearch = defineTool({
    name: "catalog_search",
    domain: "catalog",
    description:
      "Search the Unreal tool catalog by keyword. Returns compact summaries " +
      "(name, domain, one-line description) — NOT full schemas. Use catalog_describe " +
      "for a tool's parameters, then catalog_call to invoke it. Optional domain filter.",
    input: z.object({
      query: z.string().describe("Keywords matched against tool name + description. Empty lists all (filtered)."),
      domain: z.string().optional().describe("Restrict to one domain (see catalog_domains)."),
      limit: z.number().int().min(1).max(100).default(20),
    }),
    annotations: { readOnlyHint: true, idempotentHint: true },
    handler: (a) => ({
      status: "success",
      result: {
        matches: registry
          .search(a.query, { domain: a.domain, limit: a.limit + META_DOMAINS.size })
          .filter((m) => !META_DOMAINS.has(m.domain))
          .slice(0, a.limit),
      },
    }),
  });

  const catalogDescribe = defineTool({
    name: "catalog_describe",
    domain: "catalog",
    description:
      "Get one tool's full input JSON Schema + annotations (including whether it " +
      "is refused during PIE or unsupported under dry_run). Call this before " +
      "catalog_call to learn the exact parameters.",
    input: z.object({
      name: z.string().describe("Exact tool name from catalog_search."),
    }),
    annotations: { readOnlyHint: true, idempotentHint: true },
    handler: (a) => {
      const def = registry.get(a.name);
      if (!def) {
        return envelopeError(`Unknown tool: ${a.name}`, {
          code: "invalid_argument",
          hint: "Use catalog_search to find a valid tool name.",
        });
      }
      return {
        status: "success",
        result: {
          name: def.name,
          domain: def.domain,
          description: def.description,
          inputSchema: registry.inputJsonSchema(def.name),
          annotations: {
            ...def.annotations,
            blockedDuringPie: isPieBlocked(def.name),
            dryRunUnsupported: isDryRunUnsupported(def.name),
          },
        },
      };
    },
  });

  const catalogCall = defineTool({
    name: "catalog_call",
    domain: "catalog",
    description:
      "Invoke any Unreal tool by name with a params object. Equivalent to calling " +
      "the tool directly — same validation, same {status,result,error} envelope. " +
      "Lets you use the full toolset without loading every schema up front.",
    input: z.object({
      name: z.string().describe("Tool name (from catalog_search)."),
      params: z.record(z.unknown()).default({}).describe("Arguments object for the tool."),
    }),
    // Conservative: catalog_call can reach mutating tools, so not read-only.
    handler: async (a, ctx) => {
      const def = registry.get(a.name);
      if (!def) {
        return envelopeError(`Unknown tool: ${a.name}`, {
          code: "invalid_argument",
          hint: "Use catalog_search to find a valid tool name.",
        });
      }
      const parsed = def.input.safeParse(a.params ?? {});
      if (!parsed.success) {
        return envelopeError(`Invalid params for ${a.name}: ${parsed.error.message}`, {
          code: "invalid_argument",
          hint: "Call catalog_describe to see the required schema.",
        });
      }
      return def.handler(
        applyAliases(parsed.data as Record<string, unknown>, def.aliases),
        ctx,
      );
    },
  });

  return [catalogDomains, catalogSearch, catalogDescribe, catalogCall];
}
