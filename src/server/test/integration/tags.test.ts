/**
 * GameplayTags domain — register a tag, list it back, rename it, and remove it.
 * Port of tests/integration/test_tags.py.
 *
 * These ops edit the project's gameplay-tag registry (persisted to
 * Config/DefaultGameplayTags.ini), not /Game content. Each test removes the tags
 * it creates (best-effort) and every create is made idempotent by removing first.
 *
 * Pattern: arrange (clear any prior state) -> dispatch the op -> assert via tag_list.
 */

import { test, expect } from "bun:test";
import { editorSuite } from "../harness/suite.ts";

const PREFIX = "MCPTest";
const ALPHA = "MCPTest.Alpha";
const BETA = "MCPTest.Beta";
const GAMMA = "MCPTest.Gamma";

function names(listResult: any): any[] {
  return (listResult.tags ?? []).map((entry: any) => entry.tag);
}

async function drop(client: any, ...tags: string[]): Promise<void> {
  // Best-effort remove (idempotency / cleanup). Never raises.
  for (const tag of tags) {
    try {
      await client.command("tag_remove", { tag, force: true, dry_run: false });
    } catch {
      /* ignore */
    }
  }
}

editorSuite("tags", (ctx) => {
  test("tag_add_then_list", async () => {
    await drop(ctx.mcp, ALPHA);
    try {
      await ctx.mcp.expect("tag_add", { tag: ALPHA, dry_run: false });
      const listing = await ctx.mcp.expect("tag_list", { prefix: PREFIX, include_dev_comments: true });
      expect(names(listing)).toContain(ALPHA);
    } finally {
      await drop(ctx.mcp, ALPHA);
    }
  });

  test("tag_move_renames", async () => {
    await drop(ctx.mcp, ALPHA, BETA);
    try {
      await ctx.mcp.expect("tag_add", { tag: ALPHA, dry_run: false });
      await ctx.mcp.expect("tag_move", {
        from_tag: ALPHA,
        to_tag: BETA,
        rename_children: true,
        dry_run: false,
      });
      const listing = await ctx.mcp.expect("tag_list", { prefix: PREFIX, include_dev_comments: true });
      const ns = names(listing);
      expect(ns).toContain(BETA);
      // The old tag node is renamed out of the registry (a redirector is left,
      // but the source tag itself no longer enumerates).
      expect(ns).not.toContain(ALPHA);
    } finally {
      await drop(ctx.mcp, ALPHA, BETA);
    }
  });

  test("tag_remove_then_gone", async () => {
    await drop(ctx.mcp, GAMMA);
    await ctx.mcp.expect("tag_add", { tag: GAMMA, dry_run: false });
    // Sanity: it is present before removal.
    const before = await ctx.mcp.expect("tag_list", { prefix: PREFIX, include_dev_comments: true });
    expect(names(before)).toContain(GAMMA);

    await ctx.mcp.expect("tag_remove", { tag: GAMMA, force: true, dry_run: false });
    const after = await ctx.mcp.expect("tag_list", { prefix: PREFIX, include_dev_comments: true });
    expect(names(after)).not.toContain(GAMMA);
  });
});
