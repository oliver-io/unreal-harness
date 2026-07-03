/**
 * Kinematics domain — editor-world component-space transform math. Port of
 * tests/integration/test_kinematics.py.
 *
 * The kinematics ops operate on a USkeletalMeshComponent of an actor in the
 * active world. To give them a real posed component we spawn a SkeletalMeshActor
 * into the transient editor level and assign the engine SkeletalCube mesh by
 * reflection. The actor lives in the unsaved level, so editor-quit is a full
 * reset — no per-test cleanup needed.
 */

import { test, expect, beforeAll } from "bun:test";
import { editorSuite, NS as ROOT } from "../harness/suite.ts";
import { assertReady, payload, type Commandable } from "../harness/ops.ts";

const SKEL_ACTOR_CLASS = "/Script/Engine.SkeletalMeshActor";
const SKEL_MESH = "/Engine/EngineMeshes/SkeletalCube";
const MESH_LEAF = SKEL_MESH.split("/").pop() as string;
const ACTOR_NAME = "MCPTest_SkelCube";

/** Bone names of SkeletalCube, read straight off the asset (independent of
 *  whether the actor's component has the mesh assigned). */
async function boneNames(client: Commandable): Promise<string[]> {
  const info = payload(await client.expect("anim_skeletal_mesh_inspect", { path: SKEL_MESH }));
  const bones = (info.bones as Array<Record<string, unknown>>) ?? [];
  return bones.map((b) => b.name as string).filter((n) => !!n);
}

editorSuite("kinematics", (ctx) => {
  // Module-scoped fixture: spawn a SkeletalMeshActor and assign SkeletalCube to
  // its component. Returns {actor, meshSet}.
  let actor = "";
  let meshSet = false;

  beforeAll(async () => {
    const spawned = await ctx.mcp.expect("actor_spawn", {
      class_path: SKEL_ACTOR_CLASS,
      name: ACTOR_NAME,
    });
    actor = (spawned.actor as Record<string, unknown>).name as string;

    // ASkeletalMeshActor's component is `SkeletalMeshComponent`; the mesh
    // UPROPERTY on USkeletalMeshComponent is `SkeletalMeshAsset` (UE 5.1+).
    // Best-effort (command, no raise) — verified immediately below.
    await ctx.mcp.command("actor_set_property", {
      name: actor,
      property: "SkeletalMeshComponent.SkeletalMeshAsset",
      value: SKEL_MESH,
    });
    const probe = await ctx.mcp.command("kinematics_read_transform", { actor, queries: [] });
    meshSet =
      probe.status === "success" &&
      Boolean(((probe.result as Record<string, unknown>) || {}).mesh);
  });

  test("test_kinematic_read_transform", async () => {
    const result = await ctx.mcp.expect("kinematics_read_transform", { actor, queries: [] });
    expect(result.success).toBe(true);
    expect(result.world_type).toBeDefined();
    expect(result.component_to_world).toBeDefined();

    if (!meshSet) {
      console.log(
        "SKIP: SkeletalCube not assigned to the actor " +
          "(SkeletalMeshComponent.SkeletalMeshAsset reflection path) — " +
          "no posed component with a mesh to read bone transforms from",
      );
      return;
    }

    // Mesh asset is on the component: the reported mesh path must be SkeletalCube
    // and a real bone query must resolve.
    expect(String(result.mesh ?? "")).toContain(MESH_LEAF);
    const bones = await boneNames(ctx.mcp);
    if (!bones.length) {
      console.log(
        "SKIP: SkeletalCube exposes no bones (skeletal render resource is " +
          "null under -nullrhi) — no bone transform to read",
      );
      return;
    }
    const bone = bones[0];
    const posed = await ctx.mcp.expect("kinematics_read_transform", {
      actor,
      queries: [{ bone }],
    });
    const transforms = (posed.transforms as Array<Record<string, unknown>>) ?? [];
    expect(transforms.length > 0).toBe(true);
    const t0 = transforms[0]!;
    expect(t0.name).toEqual(bone);
    expect(t0.exists).toBe(true);
    expect(t0.world).toBeDefined();
    expect(t0.relative).toBeDefined();
  });

  test("test_kinematic_probe", async () => {
    if (!meshSet) {
      console.log("SKIP: SkeletalCube not assigned; kinematic_probe requires a mesh asset");
      return;
    }

    const bones = await boneNames(ctx.mcp);
    if (!bones.length) {
      console.log(
        "SKIP: SkeletalCube exposes no bones (skeletal render resource is " +
          "null under -nullrhi) — nothing to probe",
      );
      return;
    }
    const rotBone = bones[0]; // root drives the whole sub-chain
    const probeBone = bones[bones.length - 1]; // tip we measure (== root when single-boned)

    const result = await ctx.mcp.expect("kinematics_probe", {
      actor,
      rotations: [
        {
          bone: rotBone,
          rotation: { axis: { x: 0.0, y: 0.0, z: 1.0 }, angle_deg: 30.0 },
          space: "component",
        },
      ],
      probe_points: [{ bone: probeBone }],
      mode: "dryrun",
    });
    expect(result.success).toBe(true);
    expect(result.mode).toEqual("dryrun");

    const points = (result.probe_points as Array<Record<string, unknown>>) ?? [];
    expect(points.length).toBeTruthy();
    const pt = points[0]!;
    expect((pt.point as Record<string, unknown>).name).toEqual(probeBone);
    // The probe reports before/after transforms for the point (no 'exists' key).
    expect(pt.before).toBeDefined();
    expect(pt.after).toBeDefined();

    // If the editor produced a valid pose, the 30deg twist must turn the tip.
    if (result.pose_valid) {
      const ori = (pt.delta_orientation_world as Record<string, unknown>) ?? {};
      expect((ori.angle_deg as number) ?? 0.0).toBeGreaterThan(1.0);
    }
    await assertReady(ctx.mcp);
  });

  test("test_kinematic_solve", async () => {
    if (!meshSet) {
      console.log("SKIP: SkeletalCube not assigned; kinematic_solve requires a mesh asset");
      return;
    }

    const bones = await boneNames(ctx.mcp);
    if (bones.length < 3) {
      console.log(
        `SKIP: two-bone IK needs a 3-bone upper/lower/hand chain; ` +
          `SkeletalCube has ${bones.length} bone(s)`,
      );
      return;
    }
    const [upper, lower, hand] = [bones[0], bones[1], bones[2]];

    const result = await ctx.mcp.expect("kinematics_solve", {
      actor,
      chain: { upper, lower, hand },
      effector: { bone: hand },
      desired_direction: { x: 0.0, y: 0.0, z: 1.0 },
      verify: true,
    });
    expect(result.success).toBe(true);
    expect(result.solved).toBe(true);
    expect((result.chain as Record<string, unknown>)?.upper).toEqual(upper);
    expect(Array.isArray(result.resulting_rotations)).toBe(true);

    // The verification payload must prove the solve actually REACHED its own
    // hand target, not merely exist. Because the effector here IS the hand
    // bone, the solver's hand_target_world is exactly the desired tip position
    // — so the verified after-pose world location must land on it. (Validated
    // live against a 3-bone mannequin chain: residual ~1e-6 when reachable.)
    const ver = result.verification as Record<string, any>;
    expect(ver).toBeDefined();
    const afterLoc = ver.after.world.location as Record<string, number>;
    const target = result.hand_target_world as Record<string, number>;
    const residual = Math.hypot(
      afterLoc.x! - target.x!,
      afterLoc.y! - target.y!,
      afterLoc.z! - target.z!,
    );
    if (result.reachable && result.pose_valid) {
      expect(residual).toBeLessThan(1.0);
    } else {
      // Unreachable/invalid pose: best-effort — the solve must still report
      // a finite, sane residual rather than garbage.
      expect(Number.isFinite(residual)).toBe(true);
      expect(residual).toBeLessThan(1e6);
    }
    await assertReady(ctx.mcp);
  });
});
