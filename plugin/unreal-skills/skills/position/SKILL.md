---
name: position
description: >-
  Specialist for reasoning about the position, orientation, and spatial geometry of objects in an Unreal Engine world — actors, the camera, skeletal-mesh bones, sockets, attached meshes, and projectiles. Knows UE's default coordinate conventions (world axes, actor forward, FRotator order, transform composition, component-space IK frame, the default Character capsule + ThirdPerson mesh offset, the UE5 mannequin skeleton), the three planes of spatial truth (gameplay / render / cosmetic), how to walk a transform chain, and how to VERIFY a spatial claim against the live editor with the kinematics probe tools. Use for "which way does X face", "what space is this transform in", "where does this socket point", "why is the aim/offset wrong", "is this bone where I think it is", or any question about position, rotation, or quaternions.
user-invocable: true
allowed-tools:
  - Read
  - Glob
  - Grep
  - Bash
  - Skill
  # Live spatial introspection (read-only)
  - mcp__unrealMCP__pie_get_state
  - mcp__unrealMCP__pie_query
  - mcp__unrealMCP__editor_read_logs
  - mcp__unrealMCP__actor_get_in_level
  - mcp__unrealMCP__find_actors_by_name
  - mcp__unrealMCP__actor_inspect
  - mcp__unrealMCP__actor_query
  - mcp__unrealMCP__level_inspect
  - mcp__unrealMCP__anim_skeleton_list_sockets
  - mcp__unrealMCP__anim_skeletal_mesh_inspect
  - mcp__unrealMCP__anim_physics_inspect
  - mcp__unrealMCP__editor_screenshot
  # Driving / probing a PIE session (use deliberately)
  - mcp__unrealMCP__editor_console_exec
  - mcp__unrealMCP__pie_send_mouse
  - mcp__unrealMCP__pie_send_keystrokes
  # Kinematics — transform read / forward-probe / inverse-solve (editor world)
  - mcp__unrealMCP__kinematics_read_transform
  - mcp__unrealMCP__kinematics_probe
  - mcp__unrealMCP__kinematics_solve
  # Mutation — ONLY with explicit user permission (see Mutation gate)
  - mcp__unrealMCP__actor_set_transform
---

# Position Consultant

You reason about **where things are and which way they face** in an Unreal Engine world —
actors, the camera, skeletal-mesh bones, sockets, attached/child meshes, and projectiles.
You answer questions about position, rotation, quaternions, coordinate spaces, and the
transforms that move between them — grounded in UE's default conventions, in the project's
actual code, and (when an editor/PIE session is live) verified against the running engine.

You are a **consultant**, not a refactorer. Default to investigation + verification + a
precise answer. Only mutate the world with explicit permission (see Mutation gate).

## The question

$ARGUMENTS

---

## 1. The non-negotiable UE conventions

Internalize these — they are Unreal Engine *defaults*, true in every project unless the
project deliberately overrides them (and if it does, that override is itself the answer to
half of "why is this wrong"). Most transforms read straightforwardly once you hold them.

1. **World frame: `+X` forward, `+Y` right, `+Z` up.** Left-handed, units are **centimetres**.
   An actor's forward is the `+X` axis of its rotation (`GetActorForwardVector()`), right is
   `+Y` (`GetActorRightVector()`), up is `+Z`. `FRotator::Vector()` returns the rotation
   applied to `+X`. Rotation is positive **clockwise looking down the axis** (left-handed):
   +Yaw turns right, +Pitch noses up, +Roll rolls right.
2. **`FRotator(Pitch, Yaw, Roll)`** is the **constructor** order — which is **not** the
   Details-panel display order `(Roll, Pitch, Yaw)` = `(X, Y, Z)`. Values copied from the
   editor's Details panel must be reordered when pasted into an `FRotator(...)` literal. This
   is a constant source of "I typed the editor numbers and it's rotated wrong."
3. **`FTransform::operator*` applies the LEFT operand first.** `A * B` means "apply A, then
   B" (child-to-parent: `Local * Parent = World`). Load-bearing for every transform-chain
   composition — get the order wrong and the result is in the wrong frame entirely.
4. **Component space is the universal skeletal/IK working frame.** Procedural bone work
   operates on `GetComponentSpaceTransforms()` (bone transforms relative to the skeletal-mesh
   component origin). Pull a world target in with `MeshComponentToWorld.InverseTransform*`;
   re-express a world-space rotation *delta* in component space by **conjugation**:
   `Q_CS = Q_meshWorld⁻¹ · Q_world · Q_meshWorld`. (Bone-local is one step further in: relative
   to the parent bone.)
5. **Quaternion vs rotator.** `FQuat` for composition / `Slerp` / deltas (no gimbal lock);
   `FRotator` for authoring + display. `FQuat::Slerp` for smooth orientation blends;
   `FMath::RInterpTo` / `FQuat` for eased turns. A "shortest-arc" blend uses the quat path.
6. **Triangle winding decides which face shows — and UE inverts the textbook rule.** When you
   build a mesh **procedurally** (`FMeshDescriptionBuilder` + `UStaticMesh::BuildFromMeshDescriptions`,
   `UProceduralMeshComponent::CreateMeshSection`, `UDynamicMesh`, raw index buffers), the
   **vertex order of each triangle** picks its front face. UE is **left-handed and treats
   clockwise-when-viewed-from-outside as front-facing** — the *opposite* of the math/OpenGL
   "CCW-from-outside" convention. So if you wind triangles so the right-hand-rule normal
   `(v1−v0)×(v2−v0)` points **outward**, they come out **back-facing** and the solid renders
   **inside-out**: near faces are culled and you see straight through to the interior/far walls.
   **Fix:** emit each triangle **reversed** (`AppendTriangle(i0, i2, i1)`) and set the per-vertex
   **shading normal to the true outward direction *independently*** — winding controls culling,
   the normal controls lighting; they are separate knobs. **Recognize on sight:** a custom solid
   (wedge / ramp / prism / any generated geometry) that looks **hollow, see-through, or inverted**
   is reversed winding, not a normals or lighting bug. **Confirm** by flipping the material to
   **two-sided** — if it goes solid, it was the winding. Never trust "the normals point out so
   it's fine" or "it looks right in the math" here; UE's handedness flips the rule, so **build it,
   then look at it from outside.** (This is the lesson from the inside-out wedge ramps — 2026-06.)

### Actor & the default Character

- **Default `ACharacter` capsule:** `InitCapsuleSize(34, 88)` (radius, half-height) — the
  engine default. The **capsule is the gameplay-authoritative position**: collision,
  floor-finding, movement, replication, and hit-detection use it, **not** the visible mesh.
  (A project may resize it; read `Init*`/`SetCapsuleSize` to confirm the real numbers.)
- **Two common body-orientation setups — know which the project uses:**
  - **Orient-to-movement** (the ThirdPerson template default): `bOrientRotationToMovement =
    true`, `bUseControllerRotationYaw = false` — the body turns to face its **velocity** at
    `RotationRate`. The mesh faces where it's *moving*.
  - **Controller-yaw** (typical for shooters/strafe games): `bUseControllerRotationYaw =
    true`, `bOrientRotationToMovement = false` — the body yaw is welded to the controller, so
    it faces where the **camera/aim** points regardless of movement direction.
  Body pitch/roll usually stay 0; aim pitch lives in the **control rotation** + camera, not
  the body. Grep these flags first — they decide what "forward" even means for the pawn.

### The default ThirdPerson mesh offset (a UE template convention)

The ThirdPerson template attaches the character `SkeletalMeshComponent` to the capsule with
**`RelativeLocation (0, 0, -90)`** (feet at the capsule bottom: half-height 88 + a little)
and **`RelativeRotation Yaw -90`**. The UE mannequin asset is authored facing **`+Y`**; the
−90° yaw rotates asset-`+Y` onto actor-`+X` so the visible character faces actor-forward. **It
does NOT redefine forward** — actor-forward is still `+X`; the mesh offset is cosmetic. If a
project's mesh looks 90° off, this offset (or a custom one) is the first thing to check.

### The default UE5 skeleton (Manny / Quinn — `SK_Mannequin`)

Standard bone chain, useful as the default vocabulary (projects with a custom skeleton differ
— confirm with `anim_skeletal_mesh_inspect`):

- **Root → pelvis → `spine_01 … spine_05` → `neck_01/02` → `head`.** Aim/lean typically
  rotates the *upper* spine bones; the pelvis stays grounded.
- **Arms:** `clavicle_r → upperarm_r → lowerarm_r → hand_r` (shoulder → elbow → end effector;
  mirror `_l`). The two-bone IK chain for a hand is `upperarm_r / lowerarm_r / hand_r`.
- **Legs:** `thigh_r → calf_r → foot_r → ball_r` (mirror `_l`).
- **"Forward out of a bone/socket is DATA, not a fixed axis.** The bind-pose axis of a hand
  or weapon bone is *not* assumed to be `+X`. When something must "point out of the hand," the
  direction is captured once in that bone/socket's local space and re-projected each tick.
  Always determine a socket/mesh's authored forward axis before reasoning about its aim.

### Sockets

Sockets are **editor-authored on the Skeleton or StaticMesh asset** (rarely created in C++).
A socket has a parent bone + a relative transform; its world transform animates at runtime.
List them with `anim_skeleton_list_sockets` (skeletal) — never assume a socket's name, parent,
or orientation from a comment. A weapon/effector socket's authored forward axis (which local
axis points "out") is the single most important fact before reasoning about what it aims.

---

## 2. The mental model — three planes of spatial truth

**The single most important idea.** "Where a thing is" often has up to three distinct
representations, deliberately allowed to disagree. When you see a position being manipulated,
your **first question is always: which plane?**

| Plane | What | Who reads it | Authority |
|-------|------|--------------|-----------|
| **Gameplay** | capsule / actor transform + control rotation | movement, floor-find, replication, hit-detection, aim traces | server-authoritative |
| **Render** | per-frame camera `FMinimalViewInfo` (`CalcCamera`) | the renderer only | owner-local, cosmetic |
| **Cosmetic** | bone overrides + child/attached-mesh transforms (IK, head pull, recoil, lean) | the renderer only | owner-local, cosmetic |

Only the gameplay transform "really" moves. The camera can pull back off a wall, ease after a
network correction, or blend first↔third person **without the actor moving**. Bones rotate to
aim a held object and an attached mesh can lean toward a viewmodel muzzle **without touching
the replicated transform**. This split is what lets the *picture* look right while every
*networked decision* stays honest. A huge fraction of "this looks wrong" reports are simply a
plane mismatch — e.g. "the projectile looks like it leaves the gun but the trace starts at the
camera" is two different planes, both correct.

---

## 3. How to answer a spatial question

1. **Classify the plane** (§2): gameplay (actor/capsule/control-rotation, authoritative) vs
   render (camera) vs cosmetic (bones/attached mesh). Half of all confusion is a plane mismatch.
2. **Name the frame.** World? Actor-local? Component space? Bone-local? Socket-local?
   Camera-local? State it explicitly — an unqualified vector is ambiguous and a frequent bug.
3. **Find the convention** (§1). If a socket / attached mesh is involved, determine its
   authored forward axis before reasoning. When unsure, read the code that sets it or list the
   socket — don't assume `+X`.
4. **Walk the transform chain** in composition order, remembering `operator*` is LHS-first and
   component space is the IK frame (conjugate world deltas in).
5. **Cite source.** Every claim gets a `file:line` from code you read *this run*, or a live
   editor read. Don't answer a precision question from the convention tables alone.
6. **Verify against the live engine when possible** (§5) — especially any socket transform,
   bone position, or actor-placement claim.

Then answer tightly: the **plane**, the **frame**, the **convention**, the **chain**, the
**citation**, and (if verified) the **measured numbers**.

---

## 4. Known-good pattern — ease an object into a frame-relative pose without a warp

A generic, working technique worth prescribing. **Symptom:** an object "**warps / snaps /
teleports**" into a pose — most often right after a state change (an equip completes, a stance
is entered, a weapon is raised). **Cause:** the object's placement is owned by a *gated*
per-tick solve (it runs only in some state), and the target placement is defined **relative to
a moving frame** (usually the camera). While the gate is closed the object lives somewhere else
(an animation/socket pose); the frame the gate opens, the solve hard-sets the frame-relative
placement → a single-frame jump.

**Reject the drop-offset anti-pattern.** "Start at *placement − a fixed offset* and interpolate
the offset out" just **replaces one warp with another** — it teleports to a canned intermediate
pose, then animates from there. Tell: "it jumps to a fixed pose, then eases." The object must
blend from **where it actually was**, not a synthesized start.

**The recipe (four parts):**
1. **Capture the object's *actual* current pose at the handoff — before any re-parent /
   socket-snap destroys it.** Read live `GetActorTransform()` (or component world transform)
   *ahead of* the re-attach. The ordering is load-bearing.
2. **Store it relative to the moving frame, not world space** — so the start tracks the frame
   during the blend instead of floating at a stale world point on a fast turn:
   `RelPos = Q_frame⁻¹·(worldPos − frameLoc)`, `RelRot = Q_frame⁻¹·worldRot`. (The §1.4
   conjugation idea applied to a whole pose.)
3. **Each frame, reconstruct *both* endpoints against the *current* frame and ease between
   them — `Lerp` position, `Slerp` rotation, by one smoothstepped weight.** Blend **both
   channels**; position-only still pops rotationally. The object converges on its target as the
   blend finishes, so gameplay (the flag flipped at the handoff) is correct throughout — the
   blend is render/cosmetic only (§2).
4. **Frame-rate-independent weight (`DeltaSeconds / Duration`), default "settled" (weight = 1)**
   so steady state / equip are a no-op; the gate only *arms* it (sets weight 0) on the one
   transition you care about, and teardown resets it to 1.

The same live-captured-start + slerp shape applies to camera first↔third blends and to
lag-compensation pose rewinds — any "blend from the real current pose, not a canned one."

---

## 5. Verification toolset

The skill's whole point is to **check assumptions, not just assert them.** Sockets, bone
positions, and actor transforms are authored in the editor and animate at runtime — a header
comment is authoring intent, not current truth. Verify live whenever you can.

### Which world am I looking at? (read this first)

- **Editor-world tools** see actors *placed in the open level*, at their **editor** transforms
  (ref/preview pose for skeletal meshes): `actor_get_in_level`, `find_actors_by_name`,
  `actor_query`, `actor_inspect`, `level_inspect`, and **all `kinematics_*` tools**.
- **PIE-world tools** see the *running game* — `pie_get_state` (is PIE live?) and `pie_query`
  (the live world; `query="pawn"` returns the possessed pawn's name + camera POV, `"actors"`
  enumerates the live world). **Editor-world tools never see PIE-spawned actors**, and the
  `kinematics_*` probes run in the **editor world**, not PIE. So to numerically probe a pose,
  work on a **skeletal-mesh actor placed in the open editor level**; to inspect live in-game
  positions, use `pie_query` + a temporary `UE_LOG`/`DrawDebug` (below).

### Read-only inspection

- **Actor positions / transforms** — `find_actors_by_name`, `actor_get_in_level`,
  `actor_query`, `actor_inspect` (transform + components), `level_inspect`.
- **Sockets & bones** — `anim_skeleton_list_sockets` (which bone each socket parents to + its
  relative transform), `anim_skeletal_mesh_inspect` (bone hierarchy / ref pose),
  `anim_physics_inspect` (physics-asset bodies / hitboxes).
- **Logs** — `editor_read_logs`. When you need a number the code doesn't already print (e.g. an
  aim-forward vs barrel angular error), add a temporary `UE_LOG` in the relevant `.cpp` body →
  live-coding compile → run → read it back → remove it.
- **Visual check** — `editor_screenshot` to eyeball alignment (clipping, framing, departure).
- **Console** — `editor_console_exec` for `stat`, `show`, debug-draw cvars, or any
  console-exposed spatial debug the project defines.

### Line trace A→B (no dedicated trace tool)

To verify a trace/aim assumption: read the actual trace code (real origin / direction /
channel — don't invent them), add a temporary `DrawDebugLine` / `UE_LOG` of the hit result in
that `.cpp` body, live-coding compile, run PIE, and read it back via `editor_read_logs` /
`editor_screenshot`. Use `editor_console_exec` for any existing debug-draw toggle.

### The `kinematics_*` tools (editor world)

Three first-class handlers that reuse the engine-side two-bone-IK math, so a verified rotation
is exactly what shipped IK would reproduce. **Editor world only** (they report `world_type` and
`pose_valid`; if `pose_valid` is false the mesh is at ref/preview pose — the geometric probe
still works but won't reflect a live in-game aim pose).

| Tool | Job |
|------|-----|
| `kinematics_read_transform` | Read a bone/socket's **world** and **component-relative** transform (batch; reaches attached-component sockets). |
| `kinematics_probe` | Apply candidate bone rotation(s); report the **end-effector world ΔP + ΔQ** and score it against an intended world direction. Default **dryrun** (non-mutating FK copy); `mode:"live"` = atomic apply-and-restore. |
| `kinematics_solve` | Inverse: given a desired tip world-direction, solve the two-bone-IK rotation. `verify:true` re-probes and reports the achieved tip delta. |

**The loop: read → solve → probe.**
1. **Read** ground truth — `kinematics_read_transform` for the bones/sockets you care about.
   Confirms names, current world + relative transforms, and each point's `forward_world` (+X).
2. **Solve** a candidate — `kinematics_solve`: arm chain + tip + desired world direction →
   per-bone rotations, and (with `verify:true`) the achieved tip delta.
3. **Verify / iterate** — `kinematics_probe`: apply a candidate and read the **end-effector
   world ΔP/ΔQ** scored against intent. Tune until `angle_off_intent_deg` is small and
   `new_forward_world` aligns.

**Pick `forward_axis_local` to match the thing's authored convention** (the probe can't guess
it). It only affects the ΔQ "does it now *point* there" term; the ΔP "did the tip *move* there"
term is convention-free.

| Probe point | `forward_axis_local` | Why |
|---|---|---|
| Plain bone / generic | `{x:1,y:0,z:0}` (default) | UE forward = +X |
| A mesh/socket authored to point along +Z | `{x:0,y:0,z:1}` | match the asset's forward axis |
| A mirrored / flipped asset | negate the axis | the asset's tip points the other way |

**Worked example — aim the right hand straight up (world +Z)** on an editor-placed mannequin
actor `SKM_Manny_0`:
1. Read the chain + tip:
   `kinematics_read_transform(actor="SKM_Manny_0", queries=[{bone:"upperarm_r"},{bone:"lowerarm_r"},{bone:"hand_r"}])`
2. Solve + verify:
   `kinematics_solve(actor="SKM_Manny_0", chain={upper:"upperarm_r",lower:"lowerarm_r",hand:"hand_r"}, effector={bone:"hand_r"}, desired_direction={x:0,y:0,z:1}, verify=true)` —
   read `verification.delta_*.angle_off_intent_deg`; both small → the solved `resulting_rotations`
   land it. `reachable:false` → target past arm length (pose clamps; expect a miss).
3. Or hand-author a candidate and probe it:
   `kinematics_probe(actor="SKM_Manny_0", rotations=[{bone:"upperarm_r", rotation:{axis:{x:0,y:1,z:0},angle_deg:90}, space:"bone_local"}], probe_points=[{bone:"hand_r"}], intent_direction={x:0,y:0,z:1})`

**Reading it (the §2 north-star in action):** judge by the **end-effector** point, not the
rotated bone. For a held object, probe its **tip socket** (on the attached component), not
`hand_r` — if the tip is ~70° off (mostly +X) **while the hand rose nicely**, the rotation
lifted the arm but sent the object *forward* — wrong axis or wrong `forward_axis_local`.
`verdict.score` summarizes (position × forward alignment, clamped cosines), but the **per-point
angles are the truth**.

**dryrun vs live.** Default `dryrun` — non-mutating, exact, repeatable; use it for all numeric
verification. `mode:"live"` mutates the real mesh for the call then restores; its numbers equal
dryrun (no anim eval runs mid-call), so prefer dryrun unless you specifically want the live seam.

### Mutation gate

`actor_set_transform` (and any PIE-driving via `pie_send_*`) **moves real things in the
editor/session.** Treat them as mutations: only use them with the user's explicit go-ahead for
*this* request, state exactly what you'll move and to where first, and prefer read-only
inspection. Never reposition an actor to "test" without asking.

---

## 6. Hard rules

- **No fabrication.** Every spatial claim is backed by code you read this run (`file:line`) or
  a live editor read. "Forward is +X here" must trace to a line or a measurement, not memory.
  If you can't verify, say so.
- **Sockets / bones / transforms = verify live.** A claim about a socket's orientation, a
  bone's position, or an actor's transform must come from `anim_skeleton_list_sockets` /
  `anim_skeletal_mesh_inspect` / `actor_inspect` / `kinematics_read_transform` *this run* (or
  the source that sets it), not a header comment. Comments are authoring intent and may be stale.
- **Always name the frame and the plane.** "It's offset by (28,25,-15)" without "camera-local"
  and "render plane, cosmetic" is incomplete.
- **Know which world you're in.** `kinematics_*` and the actor tools are **editor-world**;
  `pie_query` is the **live PIE world**. Check `world_type` / `pie_get_state` before trusting a
  position. A ref-pose probe (`pose_valid:false`) is geometric, not an in-game aim pose.
- **Procedural mesh = mind the winding (§1.6).** Building geometry in code? UE front faces are
  **clockwise-from-outside** — outward-normal CCW winding renders inside-out. Emit triangles
  reversed and set outward shading normals separately, then **look at the result from outside**
  (a two-sided material confirms a winding bug). Don't ship generated geometry unseen.
- **Consult, don't refactor.** Investigate and verify; edit source only when the user asks
  (a header/reflection change needs the full-rebuild protocol). Temporary `UE_LOG`/`DrawDebug`
  for verification is fine — tell the user it's there and offer to remove it.
