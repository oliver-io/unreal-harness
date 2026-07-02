/**
 * Cross-domain test helpers — TS port of `tests/harness/ops.py`. Keep creates
 * idempotent so the suite re-runs against a long-lived editor.
 */

import { CommandError } from "./mcpClient.ts";
import type { Envelope } from "../../src/bridge/envelope.ts";

/** Either client (raw bridge or in-process MCP) satisfies this — the ops
 *  helpers are called with whichever the test has handy. */
export interface Commandable {
  command(type: string, params?: Record<string, unknown>): Promise<Envelope>;
  expect(type: string, params?: Record<string, unknown>): Promise<Record<string, unknown>>;
  isReady(): Promise<boolean>;
}

/** Delete an asset if present; ignore not-found / any failure. Call before a
 *  create to make the test re-runnable. */
export async function ensureAbsent(bridge: Commandable, assetPath: string): Promise<void> {
  try {
    await bridge.command("asset_delete", { asset_path: assetPath, force: true });
  } catch {
    /* ignore */
  }
}

/** Unwrap an AssetManager-style `{success, data:{...}}` envelope: return the
 *  inner `data` when present, else the result itself. */
export function payload(result: Record<string, unknown>): Record<string, unknown> {
  const inner = result.data;
  return inner && typeof inner === "object" && !Array.isArray(inner)
    ? (inner as Record<string, unknown>)
    : result;
}

/** Python-truthiness helper: `not x` is True for None, 0, "", [] and {}. JS
 *  treats [] / {} as truthy, so `assert not result.errors` (an empty list) must
 *  map to this, not `.toBeFalsy()`. Use: `expect(isFalsyOrEmpty(x)).toBe(true)`. */
export function isFalsyOrEmpty(x: unknown): boolean {
  if (!x) return true;
  if (Array.isArray(x)) return x.length === 0;
  if (typeof x === "object") return Object.keys(x).length === 0;
  return false;
}

/** Crash guard: after a risky op the editor must still be interactive. */
export async function assertReady(bridge: Commandable): Promise<void> {
  if (!(await bridge.isReady())) {
    throw new Error("editor is no longer interactive (possible crash)");
  }
}

/** First item from a list_* op, or null if empty. For content-gated tests that
 *  discover a usable skeleton/mesh/etc. */
export async function firstAssetOf(
  bridge: Commandable,
  listCommand: string,
  params: Record<string, unknown>,
  itemsKey = "",
): Promise<Record<string, unknown> | null> {
  let result: Record<string, unknown>;
  try {
    result = await bridge.expect(listCommand, params);
  } catch (e) {
    if (e instanceof CommandError) return null;
    throw e;
  }
  const keys = itemsKey ? [itemsKey] : ["items", "assets", "results", "sockets"];
  for (const key of keys) {
    const seq = result[key];
    if (Array.isArray(seq) && seq.length) return seq[0] as Record<string, unknown>;
  }
  for (const v of Object.values(result)) {
    if (Array.isArray(v) && v.length) return v[0] as Record<string, unknown>;
  }
  return null;
}
