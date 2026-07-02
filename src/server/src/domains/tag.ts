/**
 * Domain: tag — gameplay tag registry (DefaultGameplayTags.ini) CRUD.
 *
 * Port of the `tag_*` tools in `src/MCP/server.py`. Add/remove/move persist to
 * the project's INI (auto-saved); list is a read. The three mutators support
 * `dry_run` and are refused during PIE.
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";
import { dryRun } from "./_schemas.ts";

const tagAdd = bridgeTool({
  name: "tag_add",
  domain: "tag",
  description:
    "Add a gameplay tag to the project registry; persists to DefaultGameplayTags.ini " +
    "(or the named source). tag is dot-separated (e.g. Combat.Damage.Fire) with no " +
    "leading/trailing dots, empty segments, or spaces. Supports dry_run.",
  input: z.object({
    tag: z
      .string()
      .min(1)
      .describe(
        'Dot-separated tag path, e.g. "Combat.Damage.Fire". No leading or trailing dots, no empty segments, no spaces.',
      ),
    dev_comment: z
      .string()
      .default("")
      .describe("Optional developer comment surfaced in the tag picker."),
    source: z
      .string()
      .default("")
      .describe("Optional explicit tag source name. Defaults to DefaultGameplayTags.ini."),
    dry_run: dryRun.describe(
      "When True, validates but does not write to INI. Returns result.diff {added: [{tag, dev_comment, source}]}.",
    ),
  }),
  annotations: { blockedDuringPie: true },
  // dev_comment / source omitted when empty (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = { tag: a.tag, dry_run: a.dry_run };
    if (a.dev_comment) p.dev_comment = a.dev_comment;
    if (a.source) p.source = a.source;
    return p;
  },
});

const tagRemove = bridgeTool({
  name: "tag_remove",
  domain: "tag",
  description:
    "Remove a gameplay tag from the registry. Refuses (would_break_references) if the " +
    "tag has asset referencers and force=False; the error envelope's referencers field " +
    "lists the packages. force=True removes anyway. Supports dry_run.",
  input: z.object({
    tag: z.string().min(1).describe("Dot-separated tag path to remove."),
    force: z
      .boolean()
      .default(false)
      .describe("When True, removes even if referenced. Default False."),
    dry_run: dryRun.describe(
      "When True, runs the same validation but does not delete. Diff shape {removed: [{tag}], references_affected: [...]}.",
    ),
  }),
  annotations: { destructiveHint: true, blockedDuringPie: true },
  params: (a) => ({ tag: a.tag, force: a.force, dry_run: a.dry_run }),
});

const tagList = bridgeTool({
  name: "tag_list",
  domain: "tag",
  description:
    "List gameplay tags in the registry, optionally filtered by case-insensitive prefix " +
    "on the dotted path. Returns prefix, count, tags[] (each: tag, dev_comment when requested). Sorted alphabetically.",
  input: z.object({
    prefix: z
      .string()
      .default("")
      .describe("Case-insensitive prefix match on the dotted tag path. Empty returns the full registry."),
    include_dev_comments: z
      .boolean()
      .default(true)
      .describe("When True (default), each entry includes the developer comment (if set on the source)."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: (a) => ({ prefix: a.prefix, include_dev_comments: a.include_dev_comments }),
});

const tagMove = bridgeTool({
  name: "tag_move",
  domain: "tag",
  description:
    "Rename a gameplay tag in the registry, leaving an INI redirector so existing " +
    "serialized references keep resolving. The redirector is always written (no opt-out). " +
    "Supports dry_run.",
  input: z.object({
    from_tag: z.string().min(1).describe("Existing tag path."),
    to_tag: z.string().min(1).describe("New tag path."),
    rename_children: z
      .boolean()
      .default(true)
      .describe("Also rename descendant tags. Default True."),
    dry_run: dryRun.describe(
      "When True, validates but does not commit. Diff shape {moved: [{from, to}], references_affected: [...]}.",
    ),
  }),
  annotations: { blockedDuringPie: true },
  // Wire param keys are `from` / `to` (not from_tag / to_tag).
  params: (a) => ({
    from: a.from_tag,
    to: a.to_tag,
    rename_children: a.rename_children,
    dry_run: a.dry_run,
  }),
});

export const tagTools: ToolDef[] = [tagAdd, tagRemove, tagList, tagMove];
