---
name: ue-expert
description: >-
  A frontloaded knowledge base of HIGHLY SPECIFIC, known-good Unreal Engine (UE5, C++) footguns and performance patterns — the engine-level traps that produce confident-but-wrong code. Consult when writing or reviewing UE code and you want to avoid (or diagnose) an engine gotcha: "why is this slow / how do I optimize this in UE", "is this a footgun", "will this replicate / why doesn't my OnRep fire", "why is my input/ability silently doing nothing", "rubber-banding / movement prediction drift", "ragdoll is launching / floating", "GC crash on a cached pointer", "why does my SceneCapture render grey/black", "FRotator order / transform-chain / coordinate questions", "tick order / init order", "Live Coding vs full rebuild", "cook/dedicated-server/packaging traps", "PSO/shader hitch". Engine-specific and generalizable — NOT game-design, NOT a recipe library, NOT a code generator. Genericized from a production UE reference project's hard-won production learnings. Advisory + read-only: it teaches the trap and how to verify it against the live editor, it does not mutate.
user-invocable: true
allowed-tools:
  - Read
  - Grep
  - Glob
  - Bash
  - Skill
  - AskUserQuestion
  - WebSearch
  - WebFetch
  # Read-only live introspection (verify a claim against the running editor)
  - mcp__unrealMCP__project_context
  - mcp__unrealMCP__class_query
  - mcp__unrealMCP__class_inspect
  - mcp__unrealMCP__reflection_class_properties
  - mcp__unrealMCP__bp_read
  - mcp__unrealMCP__bp_inspect
  - mcp__unrealMCP__bp_brief
  - mcp__unrealMCP__catalog_search
  - mcp__unrealMCP__catalog_describe
  - mcp__unrealMCP__actor_inspect
  - mcp__unrealMCP__actor_query
  - mcp__unrealMCP__level_inspect
  - mcp__unrealMCP__anim_skeletal_mesh_inspect
  - mcp__unrealMCP__anim_physics_inspect
  - mcp__unrealMCP__kinematics_read_transform
  - mcp__unrealMCP__editor_read_logs
  - mcp__unrealMCP__pie_get_state
---

# ue-expert — the Unreal Engine footgun & performance oracle

A distilled, **engine-specific** knowledge base of the traps that make UE code *confidently wrong*: things that compile, look right, and silently misbehave at runtime, on a remote client, on a dedicated server, on a cold boot, or only at scale. Every entry is a real UE5 behavior, generalized so it applies to any project — not game design, not a how-to library.

Genericized from a production UE reference project's production learnings; it hardcodes none of that project's classes or paths.

## How to use this skill

- **Consult it while writing or reviewing UE C++/Blueprint** — match the symptom or the system you're touching to a section below, then apply the rule.
- **It is advisory and read-only.** It does not edit code or mutate the editor. It explains the trap, the *why*, and the fix shape so the caller can implement correctly.
- **Verify version-sensitive claims.** Several items name a UE version (5.3/5.5/5.7) where an API changed. When a fact is load-bearing for a decision, confirm against the **live editor** (the read tools above), the **engine source**, or current Epic docs (`WebSearch`/`WebFetch`) — don't trust memory over the installed engine.
- **This is not a substitute for measurement.** "Mind your counts" and the perf items tell you *where* to look; the profiler (`stat`, Unreal Insights) tells you what's actually hot. Optimize against data, not vibes.

## The prime directive: cost model scales by order of magnitude

Performance is a function of **how many of a thing** you have. Pick the cheapest tool that survives the count:

| Count | What's safe |
|------:|-------------|
| **1** (the player) | Blueprints fine; just avoid gratuitously expensive calls. |
| **~10** (brawler enemies) | Watch animation cost and default `ACharacter`/Blueprint overhead. |
| **~100** | Design for speed from the start; avoid Blueprint for per-frame logic. |
| **1000+** | Stop using `AActor`/`UActorComponent` entirely — plain optimized C++, batched under a manager. |

Slow code × N accumulates fast. And **optimize from day one**: if you target 60 FPS, hold ~55 FPS for ~80% of frames *throughout* development rather than crunching it at the end. This is the one place "premature optimization" advice is actively harmful — a frame budget is a design constraint, not a late polish pass.

---

## Tick

- **Blueprint tick is expensive per-instance.** ~100–150 BP ticks wired to *nothing* already burn ~1 ms on console. Put real per-frame logic in C++; use timers/timelines for time-based events on Blueprints instead of Event Tick.
- **Disable tick when idle, don't tick-and-early-return.** Toggle `SetActorTickEnabled` / `PrimaryComponentTick.SetTickFunctionEnable` at runtime. Default-disable tick (`bCanEverTick=false`) on push/event-driven components; only genuinely per-frame ones opt in.
- **Lower the rate for low-importance actors.** Not everything needs 60 Hz. Run distant/idle actors at e.g. 2 Hz via `PrimaryActorTick.TickInterval` and raise it only when "activated."
- **Logic Level-of-Detail.** Mirror mesh LOD with *behavioral* LOD — a simplified actor for distant NPCs, promoted to the full actor on approach; disable physics on moving objects too far to affect the player.
- **Batch high-count objects under a manager actor.** One manager that updates a group (projectiles, damage numbers, parts of AI) beats each object ticking itself — it enables central Logic-LOD and time-slicing (update 1/3 of the set per frame).

## Transforms, components & movement

- **Scene components are expensive to move.** Any move updates the transforms of that object's scene components *and all child scene components* — single-threaded, not fast. Large hierarchies (vehicles, modular characters) are a trap; collapse modular skeletal meshes with the engine's skeletal-mesh **merge** utility into one mesh.
- **One combined move call per frame.** Every `SetActorLocation`/`SetRotation` walks the whole transform chain (and physics if present). Aim for exactly one move per moving object per frame, and use `SetActorLocationAndRotation` / `SetActorTransform` — one combined call is ~2× faster than separate location + rotation calls.
- **`UChildActorComponent` is a *full actor*, not a cheap mesh.** Each one carries its own collision, tick, overlap/interaction registration, and GAS. Count them as full-cost actors.
- **Synchronous spawn of a complex prefab hitches in one frame.** `SpawnActor` of a BP with dozens of meshes/lights/child-actors instantiates everything that frame. Mask with VFX, or use level-instance streaming (`LoadLevelInstanceBySoftObjectPtr`) — which is also the only route when you need navmesh/AI spawners (level-only features).

## UObject lifetime & garbage collection

- **A `UObject*`/`TObjectPtr` reachable only through a non-`UPROPERTY` static/global is invisible to the GC and *will* be collected.** The moment every UPROPERTY strong ref drops, the asset is GC'd and the static pointer becomes a garbage sentinel (`0xffffffffffffffff`); the next deref crashes. Fix: `AddToRoot()` process-lifetime singletons/registries, or hold them in a `UPROPERTY`.
- **Code-minted transient definition objects need `RF_Transient` + `AddToRoot`** to survive GC while living outside any UPROPERTY.
- **Use `TWeakObjectPtr` for tracked-but-not-owned references** (delegate bookkeeping, "the thing I'm watching") so you never keep an actor alive past its own destruction — and so a stale read is a *null check*, not a dangling deref.
- **Cached `APlayerController*`/pawn pointers dangle across seamless travel** (the old PC is GC'd). Re-resolve on level load rather than caching once in `BeginPlay`.
- **Pool frequently-spawned actors.** `SpawnActor`/`Destroy` of projectiles/explosions stresses the GC and causes hitches. Object-pool anything that respawns constantly.

## Constructors, the CDO & reflection

The constructor runs on the **Class Default Object**, not the live instance — instances are created by copying CDO memory. This is the source of a whole family of "it works in editor / silently null at runtime" bugs.

- **Don't build instance-referencing objects in the constructor.** A `UInputMappingContext` (or any object holding pointers to *other* subobjects) built in the constructor captures the **CDO's** pointers, which never match the live instance's — input silently fails. Build such objects in `BeginPlay`/`OnPossess` via `NewObject`. (A pure type-descriptor like a `UInputAction` *is* safe as `CreateDefaultSubobject`; a declared-but-null `UInputAction*` is silently skipped.)
- **A raw pointer to a member subobject set in the constructor points at the CDO's copy.** Re-wire it per-instance in `PostInitProperties` (runs after the CDO→instance copy). Custom move-data containers (`SetNetworkMoveDataContainer`) often must be re-set in the constructor **and** `PostInitProperties` **and** `BeginPlay` to survive cloning.
- **Blueprint-CDO-serialized values beat constructor sets during deserialization.** If a BP subclass overrides a subobject transform/property, the deserialized value wins. If C++ must own it, re-enforce in `BeginPlay` (which also lets Details-panel overrides take effect first).
- **Never load assets during CDO construction.** `ConstructorHelpers::FClassFinder`/`FObjectFinder`/`LoadObject` in a constructor pulls the asset's dependency graph before those modules finish loading → editor crash on startup, and `ConstructorHelpers` also fails in the cook commandlet. Create components in the constructor but **load their backing assets in `OnPossess`/`BeginPlay`**; set BP class overrides via an editor subclass. CDO-unsafe property init (assigning a referenced material/asset) belongs in `OnConstruction`.
- **Cache `FindPropertyByName` results.** Resolving an `FProperty*` by `FName` every tick is wasteful; cache it and only re-resolve when the owner class changes (e.g. after a reparent / Live Coding patch invalidates it).

## Build, modules & Live Coding

- **Live Coding only hot-patches *function bodies* in existing `.cpp` files.** New/deleted files, header changes, **any** reflection-macro change (`UCLASS`/`USTRUCT`/`UPROPERTY`/`UFUNCTION`), new ctor components/replicated props/vtable shape, and `.Build.cs`/`.Target.cs`/`.uplugin`/module-dep edits all require a **full rebuild + editor restart**.
- **Live Coding invalidates cached reflection/class pointers.** A reparent or patch changes the runtime class, so a cached `FProperty*`/anim-instance class pointer goes stale — guard by re-resolving when the owner class no longer matches.
- **A live-coded anonymous-namespace `static` / `TAutoConsoleVariable` can fail to register** (the name is already owned by the original module's static; the patched static's internal ref stays null → crash on deref). Resolve CVars via `IConsoleManager` and store the `IConsoleVariable*`.
- **Warm (live-coded) success can regress on a clean rebuild + cold boot.** Cold caches behave differently (streaming, DDC, shader compile). Verify on a cold boot's *first* run, not just after Live Coding.
- **UBT auto-discovers every `.cpp` in a module.** You can split one `UClass`'s member implementations across many `ClassName_Concern.cpp` files with the header unchanged and no build-system change — the way to keep files small without churning headers.
- **`Shipping` strips the console (`~`), `stat`, `showdebug`, and Live Coding.** Any flow that leans on console commands is dead in a shipped build.

## Threading & the game thread

- **AnimInstance evaluation runs on a worker thread** (parallel anim eval). Snapshot game-thread state into the `FAnimInstanceProxy` in `PreUpdate`; keep all AnimGraph reads thread-safe; never touch live game state from the eval thread.
- **UObject mutation must be on the game thread.** Async callbacks (HTTP/REST, task graph) must marshal back to the game thread before touching UObjects — establish and rely on that convention in service clients.

## Coordinate, rotation & transform conventions

- **World frame: `+X` forward, `+Y` right, `+Z` up** (left-handed, centimeters); **actor forward is `+X`** — the same axis is both movement-forward and aim/launch-forward. `FRotator::Vector()` is that rotation applied to `+X`.
- **`FRotator` constructor order is `(Pitch, Yaw, Roll)` — NOT the editor Details order `(Roll, Pitch, Yaw)`.** Values copied out of the Details panel must be reordered in code. A very common "rotated wrong" bug.
- **`FTransform::operator*` applies the LEFT operand first:** `A * B` = "apply A, then B." Load-bearing for every transform-chain composition.
- **World→component-space rotation delta is a conjugation:** `Q_DeltaCS = Q_meshWorld.Inverse() * Q_DeltaWorld * Q_meshWorld`.
- **The UE5 Mannequin is authored facing `+Y`;** the standard −90° mesh yaw (with the ~−90 Z offset dropping feet to the capsule bottom) maps asset space to actor `+X` — it does **not** redefine "forward." Consequence: in that yawed component frame, component `+Y` is actor-forward, so a spine-lean rotated about `+Y` rolls sideways.
- **Use `FRotator::NormalizeAxis`, never `ClampAngle(x, -180, 180)`, for full-circle wrap** (e.g. `ControlYaw − ActorYaw`). `ClampAngle`'s internal `MaxDelta` collapses to 0 over a full 360° range and snaps every input to the boundary (returns 180 for input 0). Reserve `ClampAngle` for sub-360 ranges (±90 pitch).
- **Interpolate angles with shortest-arc math, not naive `Lerp`.** `Lerp(170°, −170°, 0.5)` returns 0° (the long way around the wrap). Use `NormalizeAxis(A + FindDeltaAngleDegrees(A,B) * Alpha)` for scalar yaw, `FQuat::Slerp` for full poses.
- **Sockets are editor-authored on assets; C++ only reads them** (`DoesSocketExist`/`GetSocketTransform`, guard the read). There is no programmatic socket creation. Capture a forward/blade axis in bone-local space once rather than assuming bind-pose bone axes.
- **Three planes of spatial truth deliberately disagree** — ask *which plane* before manipulating a position: **Gameplay** (capsule + control rotation, server-authoritative, the only thing that "really" moves), **Render** (per-frame camera, owner-local cosmetic), **Cosmetic** (bone overrides / child-mesh transforms, owner-local cosmetic). Cameras and bones can be eased/pulled without touching any networked transform.

## Replication & networking

**Cost model**
- **Property replication is an O(actors × clients [× properties]) poll loop.** Every net tick UE iterates each replicated actor per connection and compares each `UPROPERTY(Replicated)` to its last-sent value. Cost is dominated by the **number of replicated objects**, not properties — one `Character` with 100 replicated props is far cheaper than `Character` + 9 components with 10 each. Push Model skips the *comparison* for clean props but still iterates actors × connections.
- **Prefer RPCs for discrete, infrequent events** (damage, ability activation, score, round state): zero cost between changes vs. polling forever, and lower latency (sent immediately, not throttled to the next gather pass at `NetUpdateFrequency`). Tradeoff: no automatic late-joiner state — send a manual "current state" RPC in `PossessedBy`/`OnRep_Controller`.
- **Set replication tuning at constructor time (zero per-frame cost):** bucket actors by category and set `NetUpdateFrequency`/`MinNetUpdateFrequency`/`NetPriority`/dormancy once.
- **Net dormancy removes idle actors from the loop.** `DORM_Initial` on infrequently-changing placed actors (props, structures, resource nodes) drops them from per-tick iteration; they wake on push-model dirty marking / `FlushNetDormancy()`. Negligible at low player counts, removes thousands of actors at high counts.

**Push Model**
- **Under Push Model a write without `MARK_PROPERTY_DIRTY_FROM_NAME(Class, Prop, this)` never replicates.** Use `FDoRepLifetimeParams::bIsPushBased=true` + `DOREPLIFETIME_WITH_PARAMS_FAST`, then mark dirty at *every* mutation site. The `MARK_PROPERTY_DIRTY_*` macros live in `Net/Core/PushModel/PushModel.h`, **not** `Net/UnrealNetwork.h`.
- **`MARK_PROPERTY_DIRTY` on a dormant actor is queued, not shipped, until it wakes** — call `FlushNetDormancy()` to force the change out this tick.
- **A→B→A within one server tick transmits nothing and `OnRep` never fires.** Push model evaluates `current == lastSent` at net-tick time. If a client behavior depends on that OnRep (e.g. unequip-then-re-equip back to the same index), drive it off a *different* replicated signal whose value genuinely changes.

**OnRep semantics**
- **`OnRep_*` does not fire on the authority** (standalone/listen-server host). Any side effect normally done in the OnRep must be applied **manually on the machine that set the property**.
- **Writing a replicated property to its current value yields "no change"** and suppresses the OnRep on clients too.
- **`PostGameplayEffectExecute` does not run on simulated proxies**, so a server-side Health→0 death is never observed there. Bind client reactions off the attribute's `GetGameplayAttributeValueChangeDelegate`/`OnRep`, not the GE execute.

**Net-addressability**
- **Transient UObjects are not net-addressable.** UE resolves a `UObject*` through `FNetGUIDCache`, which needs a stable name in a persistent package or a subobject of a net-addressable actor. `NewObject(GetTransientPackage(), …, RF_Transient)` → `SupportsObject: NOT Supported` → receiver gets **null**. Fix: a custom `NetSerialize` that writes a stable key (e.g. an `FName` id) both sides resolve via an identically-built registry. **Pass `FName`, not `UObject*`, in RPC params** for the same reason.
- **A replicated `UObject*`/actor-reference field requires the referenced actor to have `bReplicates=true`** — a non-replicating actor reference arrives null.
- **`bReplicateUsingRegisteredSubObjectList`** (global `net.SubObjects.DefaultUseSubObjectReplicationList=1`) auto-opts subobjects in — no `ReplicateSubobjects` override needed (GAS ASC owners get it for free).

**RPCs**
- **RPCs cannot return values.** Keep a bool-returning mutator's semantics on authority by routing through a *private* `Server_*` RPC; the remote-client call honestly returns only "request issued." Don't put `UFUNCTION(Server)` on the public method itself.
- **Every public mutator on a server-authoritative component needs a router → `Server_*` RPC → `_Authoritative` triplet.** A remote client calling a mutator directly only changes its local mirror (diverges) and can fire untrusted side effects (HTTP, etc.). A bare early-return-on-non-authority turns every remote UI action into a silent no-op in dedicated-server mode.
- **`NetMulticast` has no `COND_SkipOwner`** — it delivers to the owning client too. Enforce skip-owner with an early-return in the *handler* (`if (GetLocalRole()==ROLE_AutonomousProxy) return;`).
- **`NetMulticast` gives free actor-relevance filtering;** replacing it with per-PlayerController `Client_*` RPCs sends to every PC regardless of relevance — a bandwidth *loss* at scale. Don't "optimize" a multicast into per-PC RPCs without reimplementing relevance.
- **Property replication is rate-bounded by `NetUpdateFrequency`; multicast RPCs are not.** To exceed the property cadence (a high-rate aim channel during fast motion), drive it through multicast RPCs.

**Iris (UE5.5+)**
- **`SetupIrisSupport(Target)` is a `ModuleRules` method (call it in `*.Build.cs`), not `TargetRules`** — it adds `IrisCore` and sets `UE_WITH_IRIS=1`. The runtime gate ANDs a global cvar (`net.Iris.UseIrisReplication=1`) with a per-driver opt-in (`+IrisNetDriverConfigs=(NetDriverDefinition="GameNetDriver",bCanUseIris=true)`).
- **Any struct with a custom `NetSerialize` makes Iris auto-generate a possibly-divergent descriptor and warn at boot** (`LogIris: Warning: Generating descriptor for struct X that has custom serialization`). Register a last-resort forwarder: `UE_NET_IMPLEMENT_NAMED_STRUCT_LASTRESORT_NETSERIALIZER_AND_REGISTRY_DELEGATES(Type)` (include `Iris/ReplicationState/PropertyNetSerializerInfoRegistry.h`).
- **GAS under Iris needs `AbilitySystem.Fix.ReplicateTagCountContainerWithIris=1`** or GAS falls back to a non-Iris path for its gameplay-tag count container. GAS attribute sets keep `DOREPLIFETIME_CONDITION_NOTIFY(..., REPNOTIFY_Always)` — the always-notify is load-bearing for the derived-attribute cascade.

**Conditions & derived state**
- **Derive non-authored attributes locally instead of replicating them** — recompute from replicated primaries in `PostGameplayEffectExecute` (server) and `OnRep_*` (client). Saves bandwidth and can't desync.
- **Derive simulated-proxy animation from replicated *velocity*, not a replicated bool.** Toggling `bIsSprinting` rapidly drifts on proxies; locomotion derived from velocity magnitude (replicated anyway) can't. Replicate only state that genuinely can't be derived (e.g. a dodge FSM enum), with `COND_SkipOwner`.
- **`COND_OwnerOnly` on an array breaks any remote observer that reads it.** If remote viewers read a replicated inventory/equipment array to drive held-weapon visuals, you can't make it owner-only for privacy without first moving the publicly-needed identity onto a separate public channel. A component shared by player pawns and world containers needs a per-instance dynamic condition.
- **Presentation state visible to remote players must be server-published as a result, never re-derived client-side.** Off-server, an item's definition can resolve to null/stub, so client-side classification ("is this armor?") silently fails. Publish the authority-computed result on a compact replicated channel (e.g. a `uint16` bitmask + `OnRep`); apply it locally on the source machine too (the OnRep won't fire there).
- **The owning `PlayerController` is not replicated to non-owning connections** — `GetController()` is null on remote proxies. Replicate the state the proxy needs rather than reaching through the controller.
- **Reparenting/attachment changes don't replicate by themselves** — a server-side parent change needs a server-auth action + multicast to resync clients. `SetReplicateMovement(true)` carries the attachment relationship (parent + socket + relative transform).

**Lag compensation (UE5 ships none)**
- **Build your own server-side rewind:** a ring buffer of hitbox transforms keyed by timestamp (~250 ms); client sends a fire RPC + sim timestamp; server clamps to `[now−window, now]`, interpolates, moves collision to the past pose, traces, restores. Rewind data is server-only and never serialized.
- **Rewind-to-past must use `ETeleportType::TeleportPhysics`** (prevents firing overlap events) and **restore actors in reverse order**. Run all trace passes inside one rewind scope; exit before spawning projectiles / applying damage.
- **Clamp client-supplied rewind/interp values server-side.** Derive rewind time from server-measured `UNetConnection::AvgLag`, not a client-claimed timestamp; floor negative rewind to 0 so a modified client can't inflate the window.
- **A listen-server / PIE host sees `AvgLag == 0` (loopback).** Latency-derived logic must detect the zero-RTT case (`AvgLag < 1 ms`) and bypass. Read the connection from `World->GetNetDriver()->ServerConnection`, **not** a sim-proxy's `GetNetConnection()` (null on sim proxies → would mis-flag every remote proxy as loopback).
- **A monotonic `uint32` server-time-ms field wraps at ~49.7 days** — promote to `uint64` (or session-relative) for long-lived servers.

## Character movement & client prediction (CMC)

- **Movement is client-predicted with server authority + reconciliation — not "local-only, networking TODO".** Never modify `Velocity` from an input handler without a Server RPC counterpart: input handlers fire only on the owning client, the server never replays the change, and it diverges. Run all velocity-modifying logic inside the CMC pipeline (`CalcVelocity`, `PhysFalling`, `OnMovementModeChanged`, `DoJump`) where the server also executes it.
- **Settings apply in `BeginPlay`, not `InitializeComponent`** (so Blueprint-CDO/Details overrides land first). But mirroring a settings struct into base-class properties in `BeginPlay` doesn't hot-update at runtime — read your own settings struct directly in physics code so unmirrored values still take effect.
- **Split custom per-move bookkeeping into "locally-controlled only" vs "all machines."** State sent per-move and restored on the server via `MoveAutonomous` (yaw rate, timers) must run only on the locally-controlled client — running it server-side fights `MoveAutonomous`. State *not* in the move data (an impulse via `PerformMovement`, stamina changes) must run on every machine or the server never applies it.
- **`MoveAutonomous` restores custom per-move fields *before* `Super::MoveAutonomous` runs `PerformMovement`** — restore from `GetCurrentNetworkMoveData()` first, clamping size/shape fields to safe ranges.
- **Quantize-before-predict.** If the client predicts from full-precision state but the server replays from quantized wire values, drift accumulates (sustained air-strafing) until it trips a correction → micro-hitch. Snap all quantized fields to wire precision *before* `TickComponent` runs physics.
- **The capsule shape must be on the wire if the client resizes it per-tick.** The server's `FindFloor` sweeps must use the same radius/half-height or floor-finding diverges → rubber-band loop. The **capsule** (not the visible mesh) is gameplay-authoritative position.
- **Move combining (`CanCombineWith`) must never combine across discrete state changes** (sprint/jump-buffer/dodge/wall-jump-consumed) — replay corrupts. Use state-sensitive thresholds; raise `MaxSavedMoveCount` (default 96) if conservative combining overflows the buffer.
- **`ServerCheckClientError` is virtual — override it for custom tolerance.** Decompose position error into XY and Z independently (vertical landing-frame disagreement shouldn't consume horizontal tolerance); return `false` when client/server disagree on `MovementMode` to skip correction for the 1–2 frames the pipelines reconcile (gravity arc vs floor snap), avoiding spurious rubber-band.
- **The owning client should NOT use engine `SmoothCorrection`** (`ENetworkSmoothingMode` is for simulated proxies). Snap the capsule authoritatively and ease only the *render camera* (accumulate the inverse teleport delta into a render-only offset, decay ~150 ms).
- **Tight-window discrete actions (~80–150 ms: wall-jump, wall-run) are too fragile for deterministic replay — predict-then-validate.** Predict locally for instant feedback, authorize via a reliable Server RPC the server gates with cheap predicates; the server does **not** re-detect (that races with position drift). Same math both sides via a shared helper.
- **To fully own air physics:** early-return `CalcVelocity` while `MOVE_Falling` (stops engine braking/clamping) **and** skip `AddMovementInput` in the air (stops a stray `Acceleration` leaking in). Gravity is unaffected (`PhysFalling` applies it before `CalcVelocity`).
- **A buffered/re-jump on landing must bypass `DoJump`.** In UE5.3+ `DoJump` calls `CanJump()`, which requires `bPressedJump==true`; a jump armed mid-air never sets it, so the jump silently fails. On the `OnMovementModeChanged` landing transition, write `Velocity.Z` directly and `SetMovementMode(MOVE_Falling)` *before* `PhysWalking` applies friction.
- **Crouch-jump needs a `CanJumpInternal_Implementation` override** (return true even when crouched) + direct Z write, skipping `Super::DoJump()`. Override `CanCrouchInCurrentState()` to allow crouch while falling.
- **UE's crouch compensation pushes the 3P mesh UP by the half-height adjustment — wrong in the air** (head punches through ceilings). Detect the air-crouch case in `OnStartCrouch` and reset the mesh relative-Z to the CDO default.
- **`SlideAlongSurface` eats horizontal speed on ramp impacts** proportional to incline. Cache pre-landing horizontal velocity (in `PhysFalling` before `Super`) and restore on the re-jump to keep ramp speed equal to flat.
- **`GetMaxSpeed()` returning current velocity while falling is a trap** — return a fixed ceiling for the falling case. Apply equipment/skill multipliers identically on owner + server so predicted and authoritative max speed agree.
- **A pawn whose yaw should track its controller needs `bUseControllerRotationYaw=true`** — `APawn` defaults it OFF, so without it (and with orient-to-movement off) the body stays frozen at spawn rotation while aim IK tracks a focus → every action whiffs at a constant bearing error.
- **UE5.5 deprecations that become compile errors:** `CrouchedHalfHeight` field → `Set/GetCrouchedHalfHeight()`; `DoJump(bool)` → `DoJump(bool, float DeltaTime)`. AI path-following locomotion needs `NavMovementProperties.bUseAccelerationForPaths` since 5.5 (else a pathing NPC slides in idle pose).
- **`>120 Hz server tick requires an engine-source change** to the `UNetConnection::Tick` clamp; otherwise it's config-driven (`NetServerMaxTickRate`). Sub-step alignment (`MaxSimulationTimeStep`/`MaxSimulationIterations` matched to server tick) keeps corrections rare and small.

## Collision & tracing

- **Stock `ECC_Visibility` has Pawns set to `Ignore`** on the Pawn/CharacterMesh/Ragdoll profiles — a Visibility trace passes *through* live characters. Correct for camera occlusion / AI sight / ground probes / splash-damage LOS; **wrong for "I'm trying to hit a thing."**
- **Use ONE dedicated combat trace channel for all "trying to hit" queries.** Register a project channel (e.g. `AimTarget` on `ECC_GameTraceChannel2`) with `DefaultResponse=ECR_Block` so everything blocks it unless explicitly overridden via `+EditProfiles=` (overlap/trigger/water → Overlap). One channel serves aim trace, blade sweep, hitscan, projectile trace; classify the hit by actor cast. Split rule: **combat channel for "hit a thing," Visibility for "see/find geometry past characters."** Never co-opt an unrelated object channel (e.g. Projectile) as a workaround.
- **Collision identity should be state-invariant.** "What am I for hit-testing" is a property of the entity, not its sub-state. On death, swap the mesh *profile* (`CharacterMesh`→`Ragdoll`, both blocking the combat channel by inheritance) rather than juggling per-channel responses.
- **`SweepSingleByObjectType` for WorldStatic/WorldDynamic also hits `OverlapAllDynamic` interaction spheres** unless those profiles are explicitly set, a latent bug in any "wall pass." A single-channel sweep + actor-cast classification removes it.
- **Mesh-bone narrowphase (`LineTraceComponent`) bypasses the channel system entirely** — use it *after* a capsule hit confirms a character, to trace against the physics asset for head-vs-body accuracy.
- **`SetCanEverAffectNavigation(false)` on cosmetic/decorative meshes** keeps them out of navmesh-generation cost.

## Animation

- **Keep anim graphs on the fast-path; prefer state machines over blends.** Animation trees are a common bottleneck. Every node should show the fast-path (lightning icon) — only pure member-variable access stays on it; any node-graph logic drops off. Minimize total blends; a state machine is often faster than equivalent blends.
- **Reading bone/socket transforms the same frame requires the mesh to have ticked first** — add `AddTickPrerequisiteComponent(Mesh)` so IK sees current bone data.
- **Tick-group discipline for pose-dependent work.** The AnimBP evaluates at `TG_PrePhysics`, so work that must read the *posed* skeleton (IK, attachment riding, lag-comp bone capture, cosmetic drivers) runs at `TG_PostUpdateWork`/`TG_PostPhysics`. If IK lags a fast target, move the *mesh's* tick earlier so the solve reads up-to-date target transforms.
- **One-frame anim-order delay:** a component at `TG_PostPhysics` writing state an AnimBP reads at `TG_PrePhysics` is visible next frame — <17 ms at 60 Hz (invisible), but at 30 Hz add a tick prerequisite and move the work to `TG_PrePhysics`.
- **Procedural IK that reads/writes bone transforms needs `AlwaysTickPoseAndRefreshBones` + URO off** so bone transforms are always current — including on a dedicated server, where IK must still run for authoritative aim/hit geometry.
- **Editing component-space bone transforms requires re-mirroring + `MarkRenderDynamicDataDirty()`** (obtain the buffer via `GetComponentSpaceTransforms()`, mirror edits in, mark dirty) or the edit is overwritten by the next eval.
- **Leader-pose follower meshes copy the leader at `OnBoneTransformsFinalizedMC`** (during `EndPhysicsTickComponent`). A procedural bone edit made *after* that (e.g. weapon IK at `TG_PostUpdateWork`) lands on the invisible driver mesh and never renders on the follower — run body-bone edits on the bone-finalize hook.
- **An armed upper-body overlay must collapse its alpha when the source pose is null** — an overlay rooted at a sequence player with a null source renders ref-pose (A-pose) arms on every wielder.
- **Two-bone analytic IK (law of cosines) is bone-length-conserving; pass an explicit pole/bend vector** to keep the bend in the object frame. Inferring bend from the *animated* mid-joint leaks the animated pose → desync under motion. For a held grip, solve once and FREEZE rigid-to-the-hand (per-frame re-solve jitters fingers); gate the freeze on a validity signature or a transient "stable garbage" pose gets latched.
- **`UMotionWarpingComponent` warp targets are inert without a matching montage warp notify** — `AddOrUpdateWarpTarget("X")` only bends root motion if the author placed a matching notify.
- **`PlayMontageAndWait` defaults its mesh to `GetMesh()` (the 3P body) — that is correct; do not override `AbilityActorInfo->SkeletalMeshComponent` to a 1P mesh.**

## Ragdoll & physics assets

Ragdoll "launch" / "popped balloon" on a non-Mannequin mesh is **almost always a depenetration feedback loop, not the ragdoll code**. Diagnostic signature: pose is clean at `SetSimulatePhysics(true)` (bodies dynamic, velocities ≈0), then velocity *grows* frame-over-frame faster than a clamp can remove it — a kinematic collider co-located with a simulating body shoves it out every frame.

- **Bone-attached gameplay capsules are invisible to the PhysicsAsset self-collision table.** Separate `UPrimitiveComponent`s socketed onto bones (mount volumes, per-bone hitboxes) aren't in the PhAT table, so no table fix touches them — on ragdoll each kinematically chases its bone and depenetrates the dynamic body. Fix: **disable collision on every non-ragdoll-mesh `UPrimitiveComponent` on the pawn before the mesh starts simulating.**
- **`SetDisableCollision` on a constraint only reaches *adjacent* (constrained) body pairs.** Non-adjacent overlapping bodies with absurd authored masses depenetrate every frame and launch the corpse. Complete the disable table for **every** pair (`UPhysicsAsset::DisableCollision(i,j)` over all i<j) then `RecreatePhysicsState`. The default Mannequin never needs this — its PhAT table is already complete.
- **A bone-anchored kinematic "walk-into" proxy must be `QueryOnly`, never `QueryAndPhysics`** — a `QueryAndPhysics` capsule blocking `ECC_Pawn` depenetrates the very ragdoll body it rides. `QueryOnly` still blocks player movement (a query sweep) but takes no part in physics. Correspondingly override the corpse mesh's `ECC_Pawn → Ignore`.
- **A synchronous velocity clamp the same frame as `SetSimulatePhysics` reads a stale pre-solve (~0) velocity under Chaos and no-ops.** Cap inherited bone velocity on a post-solve pass the *next* frame(s).
- **Never inject a per-tick force into a settling ragdoll** — a per-frame velocity source (augmented-gravity `AddForce`) pins body speed above the settle threshold forever, so the detector never fires. World gravity + a one-shot damping clamp settle it on their own.
- **Defensive fixes for non-Mannequin PhysicsAssets authored for a kinematic rig:** `SetEnableGravity(true)` on every body (some author gravity off → corpse floats); `SetDisablePostProcessBlueprint(true)` (a foot/spine IK post-process runs *after* the physics blend and yanks legs to IK targets); zero every constraint drive/motor (they pump energy into passive bodies); normalize wild masses via `SetMassScale`.
- **Freezing a settled ragdoll needs a held-pose source** — a kinematic body is animation-driven, so once physics stops the mesh snaps to ref-pose (A-pose). Snapshot the settled pose (`FPoseSnapshot`) while still simulating, swap onto a minimal anim instance that replays it, `SetAllBodiesSimulatePhysics(false)`, then force one synchronous anim eval (`TickAnimation(0)`+`RefreshBoneTransforms`) *after* the flip to avoid a one-frame A-pose flash. Kinematic bodies also let corpses rest on each other instead of jittering.
- **Stationary AI corpses keep re-triggering proxy net smoothing.** An AI that ragdolls but stays `bReplicateMovement` makes the server stream its now-stationary transform every tick, perpetually re-triggering proxy `NetworkSmoothingMode` — the smoothed offset fights the local ragdoll and hurls the carcass. Fix: on death `SetReplicateMovement(false)` on authority + CMC `NetworkSmoothingMode=Disabled` everywhere (do NOT use dormancy — it would also stop the replicated death bit).
- **Death from a GAS attribute (Health→0) must bind on every machine and guard on real engine state.** Remote-client PCs aren't replicated for other players' pawns, so the `OnRep_Controller`→init path skips and the death delegate never binds — force-bind in `BeginPlay` on `!HasAuthority()`. Guard ragdoll re-entry on `GetMesh()->IsSimulatingPhysics()`, **not** a replicated `bDead` flag (a `bDead` guard makes the `OnRep_Dead` path early-return on remote clients, since the bit arrives before the helpers run).
- **`AttachChildren`/`GetAttachedActors` may be empty when `AttachmentReplication` arrives on a dedicated client** — a held item is visually attached but the iterator returns nothing, so a death handler can't find it to drop it. Recover in the held actor's `OnRep_Owner`.
- **`ApplyDeathImpulse` velocity is multiplied by bone body mass** — pass desired linear velocity (cm/s), not raw force; early-out on `!IsSimulatingPhysics()` so a hit the same frame as the lethal GE (or on a settled kinematic corpse) no-ops.

## GAS (Gameplay Ability System)

- **Granted-ability lifecycle must be symmetric.** Abilities granted on equip (`ASC->GiveAbility(Spec)`) must have their returned handles stored and explicitly removed on unequip/break. The grant is the prerequisite for `TryActivateAbilitiesByTag`/`ByClass` to find anything — an input binding with **no granted spec silently does nothing**.
- **Route damage through a meta-attribute, never write a vital attribute directly from a GameplayEffect.** Damage GEs apply additively to an `IncomingDamage` meta-attribute; `PostGameplayEffectExecute` runs mitigation, applies the survivor to `Health`, and zeroes the meta. Writing `Health` from a weapon GE bypasses the whole pipeline.
- **Drive ability timing from `UAbilityTask_WaitDelay`/`WaitGameplayEvent` but always include a timer/montage-null fallback** so the ability still works without editor-authored AnimNotifies.
- **An AI ability driver must guarantee the press/release pair an input system would.** A "hold" ability waits for a release players' input guarantees; an AI must deliver a pending release in `ExitState` (state re-selection orphans the chamber → arm frozen, later activations rejected) and self-heal any orphan before re-activating.

## Rendering, SceneCapture, Niagara & 2D

- **Off-screen `SceneCaptureComponent2D` never drives the texture streamer** — a single off-screen capture renders the mesh's diffuse at engine *default* (white/grey). A clean *silhouette* proves geometry, not that the material rendered. Force textures resident (`SetForceMipLevelsToBeResident`) and wait on `IsFullyStreamedIn()`. Diagnose by logging average RGB of non-black pixels: neutral grey = default material, colored = real texture, pure 255 = blown out.
- **`ShowOnlyActors` / `PRM_UseShowOnlyList` filters primitives, NOT lights** — a show-only capture is still lit by the level's sun + skylight (can blow to white). Disable scene lights via show-flags, or sidestep lighting by capturing `SCS_BaseColor`.
- **`SCS_SceneColorHDR`/`FinalColorLDR` force an opaque-black background without project-wide alpha propagation.** The RT's own `ClearColor` only applies to GBuffer-style sources; post-processed sources resolve the engine scene-color buffer (cleared opaque black) regardless. For transparent UI cutouts, capture `SCS_BaseColor` and key the background out on the CPU (luminance→alpha) — base color also removes lighting/exposure as variables. (`r.PostProcessing.PropagateAlpha` is a game-wide change.)
- **Never single-shot a `SceneCapture` in `BeginPlay` on a cold cache** — the asset's textures/shaders aren't even in-flight, so no synchronous flush fixes it. Do a throwaway "kick" capture, then defer the real one until `IsFullyStreamedIn()` for all textures AND `GShaderCompilingManager` is idle, with a frame-budget safety net. Verify capture features on a cold boot's *first* run.
- **Drawing TEXT into a render target (worldspace "screen"):** `FCanvasTextItem` + a composite `FSlateFontInfo` (the path UMG/HUD text uses) renders **nothing** into an offscreen RT via `BeginDrawCanvasToRenderTarget` — the Slate font atlas isn't part of that game-thread canvas pass. Use `UCanvas::K2_DrawText` + a **runtime `UFont`** instead (import the .otf/.ttf as a runtime UFont). The HUD works because it paints onto a live Slate surface; an RT is a different surface.
- **A runtime UFont drawn via `K2_DrawText` rasterises at `UFont::LegacyFontSize`, then the canvas `Scale` stretches that bitmap** — so a small base size blown up to fill the RT is blurry. Set `Font->LegacyFontSize` near the on-RT display height (transient, in code) so it draws at ~1× and only ever down-scales → crisp.
- **A worldspace "screen" made of a TWO-SIDED thick slab paints the RT on BOTH the front and back faces.** Viewed at an angle you see two copies of the content, offset by the slab thickness and mirrored, which merge into garbage at distance (a "2" reads as "8"). **Use a single-sided material** (or a single-quad Plane), and cant the panel so its front face always faces the viewer. This is usually the real cause of "doubled / garbled" RT-screen text — not the font.
- **`AutoGenerateMips` on a Canvas-drawn RT does NOT repopulate the mip chain from the draw** — the higher (minified) mips stay at the clear value, so the screen reads **BLANK at distance** and only appears up close (a near camera samples mip 0). Leave the RT `mips=false`; tame distance aliasing by reducing high-frequency content (a SOLID-dominant glyph, not a fine dither as the main signal) instead.
- **Bloom (emissive >1) merges bright thin features on a distant screen** — same trap as the Niagara "huge ribbon": keep the glyph emissive ≲1 so a "2" doesn't bloom-close into an "8". Reserve high glow for dim fills/accents.
- **VERIFY a worldspace screen the right way.** A flat RT dump — or compositing the material to a flat full-res RT via `DrawMaterialToRenderTarget` — **lies**: it shows crisp text while the in-world panel garbles, because it bypasses bloom, minification, and mesh-face doubling. Capture the **editor viewport WITH bloom** framed on the panel at gameplay distance/angle (`editor_screenshot mode:editor` after framing the level viewport), or look in PIE. Never sign off an RT-screen from the flat texture.
- **Tint a UMG image via the brush's `TintColor`, not the widget's `ColorAndOpacity`** (the latter doesn't reliably multiply the image).
- **Niagara ribbon (SpawnPerUnit) keys off per-FRAME component displacement** → fast emitters undersample and produce sparse/absent trails. Treat Niagara as a dumb geometry generator: spawn it *unattached* and walk the component along the path yourself in fixed-distance increments (matching the emitter's Spawn Spacing — they're one contract split across two assets), advancing one sim sub-tick per increment.
- **Niagara sub-tick advances must use a negligible delta time** — the sub-steps register *movement* (fire SpawnPerUnit), not *age* particles; a real dt double-ages the ribbon. The normal per-frame tick owns aging/fade.
- **PIE caches the compiled Niagara system — emitter asset edits (width/lifetime/spawn-spacing) need a PIE restart;** material edits DO hot-apply. An apparent "huge ribbon" is usually **bloom** (emissive >1), not geometry — keep emissive <1 for a thin look.
- **Per-client Material Instance Dynamic (MID) gives per-viewer cosmetic variation at zero bandwidth** — create a MID per spawn per machine, drive params in C++, route into a Niagara ribbon via `User.RibbonMaterial`.
- **Never spawn cosmetic FX on a dedicated server** (it renders nothing) — guard on net mode. Per-client cosmetics (trails, MIDs) are built on every rendering machine from replicated motion, never spawned/replicated by the server. Spawn replicated **projectiles on the camera/aim ray**, not a viewmodel/body socket (those resolve to ref pose on a dedicated server and are invalid replicated origins); "looks like it left the weapon" is an owner-only cosmetic layer.
- **Render thousands of remote players via multi-tier LOD;** GPU-instanced static meshes are the only tier where crowd-sim is appropriate. Full skeletal (0–30 m) → simplified mesh (30–80 m) → GPU-instanced "walking figure" (80 m+, positions still network-driven) → aggregate indicators (200 m+). Don't use autonomous crowd/agent sim to drive *positions* of players with known ground-truth — it fights the incoming data and drifts/snaps.

## Asset loading & soft references

- **`LoadObject`/path-string loads must include the object-name suffix** (`/Game/Path/Asset.Asset`); a package-only path returns null and the mesh "silently never appears."
- **An unbound `TSoftObjectPtr` is a silent no-op** — an empty asset slot costs nothing and never errors, so a typo'd/forgotten binding fails silently.
- **StateTree assets must be re-saved/recompiled after any C++ change to a task/condition instance-data struct or schema type**, or they silently fail reference validation at load (`LogStateTree: Warning: … could not compile. Please resave`).

## Serialization

- **Network `NetSerialize` (binary `FArchive`, every tick) and persistence serialization (JSON snapshot, save/load) are entirely different lifecycles — don't conflate them.** Different purpose, code, cadence; keep persistence serialization as freestanding free functions (`Serialization → Game` one-way dependency, no UObjects).
- **`FJsonObjectConverter` auto-maps USTRUCT trees to/from JSON via UHT reflection** — `UStructToJsonObjectString` with the camelCase flag yields `characterName`; deserialize into a `TOptional` that's empty on parse failure. No custom parser needed.
- **Capture GAS attribute *base* values via `ASC->GetNumericAttributeBase()`, restore via `SetNumericAttributeBase()`** (skip invalid `FGameplayAttribute`s); recompute derived attributes on restore.
- **One DB table written by two never-reconciled writers corrupts itself** — if one writer sets an authoritative column and another does `INSERT OR REPLACE` omitting that column, the second silently nulls it. Make one system the single source of truth.

## AI, perception & StateTree

- **The StateTree orchestrates (shallow, asset-light); C++ thinks** (target/threat selection, memory, action semantics). Keep judgment out of the decision graph. (See `/npc_logic` for the full layered model.)
- **`AIPerception` sight cone is sourced from `GetActorEyesViewPoint` rotation, not actor-forward** — it follows view rotation via `GetLocationAndDirection`, so a gaze/look-around offset steers perception with the head. Conversely, a mesh that sits in its wielder's eye line on `ECC_Visibility` (which sight runs on) **blinds the wielder** — e.g. a raised shield must ignore `ECC_Visibility`.
- **Without a team interface, UE's attitude solver resolves `NoTeam vs NoTeam` as Friendly** and silently drops hostile-only stimuli (e.g. hearing). Implement `IGenericTeamAgentInterface` with a concrete team id.
- **Gate movement-based perception at the producer, not per-listener.** A stimulus-source component that registers its owner only while velocity exceeds a threshold (hysteresis, low tick rate, unregister on EndPlay) makes cost scale with the number of *moving* actors, not listeners × actors — and keeps LOS/cone work in UE's native pipeline.
- **Deferred-spawn AI whose identity selects team/registration/tree must call `SetInstanceIdentity` before `FinishSpawning`** — post-possession changes re-register but don't swap the already-loaded StateTree.

## Cook, packaging & dedicated server

- **The Epic Launcher engine cannot build Server targets** (no `RunUBT.bat`/`RunUAT.bat`) — a **source engine build** is required. The binary that loads content and the editor that cooked it must be the **same engine build**; mixing launcher-cooked content with source-built binaries gives "Corrupt data found" serialization errors.
- **A dedicated server has no renderer — cook the `WindowsServer` (or Linux) platform separately.** The server reads `Saved/Cooked/WindowsServer/`, not `…/Windows/`. There's typically no `TargetType.Client`; the standalone client is the plain `Game` target (so client packaging uses `-configuration`, not `-client`/`-clientconfig`).
- **Runtime-loaded assets are invisible to the cook's map-reference trace.** Anything loaded by C++ at runtime (`LoadObject`, `ConstructorHelpers`, `TSoftObjectPtr`) must be under `+DirectoriesToAlwaysCook` or registered with the Asset Manager via `UPrimaryDataAsset` — else it's missing at runtime. The raw `-run=cook` commandlet doesn't reliably honor `DirectoriesToAlwaysCook` (re-pass each as `-CookDir=`; the UAT path honors the ini) and **returns non-zero even on success** — verify by the output dir, not the exit code.
- **Editor-only plugins must be excluded from the cook** — `-AdditionalCookerOptions="-DisablePlugins=<EditorPlugin>"`, or it drags its assets in.
- **UE uses UDP — TCP port-polling won't detect server readiness.** Watch stdout for `"Engine is initialized. Leaving FEngineLoop::Init"`.
- **The default server map is `/Engine/Maps/Entry`** — set `ServerDefaultMap` to the real map. **Shipping** strips console/`stat`/Live Coding; distribution needs `-pak` (+IoStore), and tree-shaking only includes assets reachable from the build's maps (migrate runtime-loaded assets to the Asset Manager). Linux server cross-compile needs the separate UE Linux toolchain (`LINUX_MULTIARCH_ROOT`), not pulled in by `Setup.bat`.

## Shader / PSO pipeline cache (cooked builds)

- **First-render PSO compile stalls the render thread (50–500 ms).** The first time a unique material × vertex-factory × render-pass combo renders on a given GPU, UE compiles a fresh Pipeline State Object on the render thread → a visible hitch. A bundled `.upipelinecache` pre-compiles them at boot so gameplay starts warm.
- **The pipeline cache only helps cooked/shipping builds — PIE never consumes it.** Don't expect it to fix editor hitches.
- **It's per-platform and must be re-recorded on changes** — a Windows `.upipelinecache` does nothing for Linux/server/console. Re-record per platform and whenever a shipped material/shader combo is added, the renderer feature set changes (Lumen/Nanite/MSAA/mobile), or the UE version bumps. Recording must traverse the *superset* of all shipped rendering states or you still hitch in production.
- **`r.*` cvars in an ini live under `[SystemSettings]`** (not a bare `[ShaderPipelineCache]`). Render cvars like `r.ShaderPipelineCache.Enabled/.LogPSO/.SaveBoundPSOLog` belong there; clear the local shader DDC (`%LOCALAPPDATA%\UnrealEngine\<Version>\DerivedDataCache`) when verifying a cold boot.

## Editor & PIE gotchas

- **The editor's background CPU throttle (`bThrottleCPUWhenNotForeground`) drops unattended PIE to ~3 FPS.** Every per-frame system then lies — one-frame control-rotation staleness becomes a large constant aim error; game-time and wall-clock diverge. Disable it for automated/MCP-driven PIE; if timing data looks quantized to ~0.33 s, check this first. Time-gated automation should count *game* time (`UWorld::GetTimeSeconds`), not wall clock.
- **A dead-fallback config twin from an early-return registry lookup looks authoritative but is dead code.** A common UE pattern — an actor builds config by first looking it up in a registry and early-returning if found, with an in-actor builder as a "defensive fallback." In normal play the registry copy (populated eagerly before any actor spawns) wins, and the in-actor list is dead code that *looks* authoritative — **editing it produces no effect.** Keep one source of truth, or make the dead twin loudly self-identify where an editor would change it.
- **"Convert Selection to Blueprint Class" bakes relative transforms** — compose actors in a normal level, then collapse them into one self-contained BP preserving relative transforms, without ever editing in the BP viewport. A sanctioned authoring path for non-modular prefabs.
- **An asset save that fails with "PromptForCheckoutAndSave failure (SCC errors)" is usually NOT source control — check the log for `Error Code 32`** (Windows sharing violation: another process has the `.uasset` open). The common cause is a **second editor instance** (e.g. launching one when another is already running — the tell is the bridge being ready instantly). Only one editor binds a given port; the duplicate still holds every `.uasset` open and blocks saves. Find the non-bridge editor PID (the one NOT owning the bridge port) and kill it. The file isn't lost — the failed save/delete are no-ops.
