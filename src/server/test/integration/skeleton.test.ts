/**
 * Skeleton domain — socket CRUD on a USkeleton, read back via the list op.
 * Port of tests/integration/test_skeleton.py.
 *
 * Self-contained: operates on the engine's stock skeleton
 * `/Engine/EngineMeshes/SkeletalCube_Skeleton` (confirmed present in every
 * install). Sockets are added/modified/removed entirely IN MEMORY — we never
 * call save_asset on the engine package, so nothing on disk is mutated; each
 * test restores the skeleton to its starting socket set via the add/remove
 * pair and is idempotent (a stale test socket from a crashed prior run is
 * removed first).
 *
 * Pattern for every test: arrange prerequisite state -> dispatch the op (raises
 * on a non-success envelope) -> assert the resulting state via
 * list_skeleton_sockets.
 */

import { test, expect, beforeAll } from "bun:test";
import { editorSuite } from "../harness/suite.ts";
import { assertReady } from "../harness/ops.ts";
import type { Commandable } from "../harness/ops.ts";

// Stock engine assets (present out of the box; never saved/mutated on disk).
const SKELETON = "/Engine/EngineMeshes/SkeletalCube_Skeleton";
const MESH = "/Engine/EngineMeshes/SkeletalCube";
const SOCKET = "MCPTestSocket";

// ── local helpers ────────────────────────────────────────────────────────────

/** Idempotency guard: drop a leftover test socket, ignoring 'not found'. */
async function removeSocketQuietly(bridge: Commandable, name: string = SOCKET): Promise<void> {
  try {
    await bridge.command("anim_skeleton_remove_socket", {
      skeleton_path: SKELETON,
      socket_name: name,
    });
  } catch {
    /* ignore */
  }
}

async function socketEntries(bridge: Commandable): Promise<Record<string, unknown>[]> {
  const result = await bridge.expect("anim_skeleton_list_sockets", { skeleton_path: SKELETON });
  const sockets = result.sockets;
  expect(Array.isArray(sockets)).toBe(true);
  return sockets as Record<string, unknown>[];
}

async function socketNames(bridge: Commandable): Promise<Set<unknown>> {
  const entries = await socketEntries(bridge);
  return new Set(entries.map((s) => s.socket_name));
}

async function addTestSocket(
  bridge: Commandable,
  bone: unknown,
  locationZ = 10.0,
): Promise<Record<string, unknown>> {
  return await bridge.expect("anim_skeleton_add_socket", {
    skeleton_path: SKELETON,
    socket_name: SOCKET,
    bone_name: bone,
    location_x: 0.0,
    location_y: 0.0,
    location_z: locationZ,
    rotation_pitch: 0.0,
    rotation_yaw: 0.0,
    rotation_roll: 0.0,
    scale_x: 1.0,
    scale_y: 1.0,
    scale_z: 1.0,
    dry_run: false,
  });
}

editorSuite("skeleton", (ctx) => {
  // ── fixture: a real bone name on the SkeletalCube skeleton ─────────────────
  // Discovered from the skeletal mesh's ref skeleton (always available — it is
  // not render-data gated, so it works under -nullrhi). Falls back to "Root"
  // if discovery comes back empty (the add handler defers bone validation, so a
  // plausible name still round-trips through list_skeleton_sockets).
  let socketBone: unknown;
  beforeAll(async () => {
    const info = await ctx.mcp.expect("anim_skeletal_mesh_inspect", { path: MESH });
    const bones = (info.bones as Record<string, unknown>[] | undefined) || [];
    socketBone = bones.length ? bones[0]!.name : "Root";
  });

  // ── tests ────────────────────────────────────────────────────────────────
  test("test_add_skeleton_socket_appears_in_list", async () => {
    await removeSocketQuietly(ctx.mcp); // start from a known-clean state
    const result = await addTestSocket(ctx.mcp, socketBone, 12.0);
    expect(result.success).not.toBe(false);
    try {
      const names = await socketNames(ctx.mcp);
      expect(names.has(SOCKET)).toBeTruthy();
      // the listed entry should report the bone we attached to
      const entries = await socketEntries(ctx.mcp);
      const entry = entries.find((s) => s.socket_name === SOCKET)!;
      expect(entry.bone_name).toEqual(socketBone);
    } finally {
      await removeSocketQuietly(ctx.mcp); // leave the engine skeleton as we found it
    }
  });

  test("test_modify_skeleton_socket_updates_value", async () => {
    await removeSocketQuietly(ctx.mcp);
    await addTestSocket(ctx.mcp, socketBone, 5.0);
    try {
      await ctx.mcp.expect("anim_skeleton_modify_socket", {
        skeleton_path: SKELETON,
        socket_name: SOCKET,
        location_z: 42.0,
      });
      const entries = await socketEntries(ctx.mcp);
      const entry = entries.find((s) => s.socket_name === SOCKET)!;
      expect(Math.abs(Number(entry.location_z) - 42.0)).toBeLessThan(1e-3);
    } finally {
      await removeSocketQuietly(ctx.mcp);
    }
  });

  test("test_remove_skeleton_socket_is_gone", async () => {
    await removeSocketQuietly(ctx.mcp);
    await addTestSocket(ctx.mcp, socketBone);
    expect((await socketNames(ctx.mcp)).has(SOCKET)).toBeTruthy();

    const result = await ctx.mcp.expect("anim_skeleton_remove_socket", {
      skeleton_path: SKELETON,
      socket_name: SOCKET,
    });
    expect(result.removed_socket).toEqual(SOCKET);
    expect(result.remaining_sockets).toBeDefined();
    expect((await socketNames(ctx.mcp)).has(SOCKET)).toBeFalsy();
    await assertReady(ctx.mcp);
  });
});
