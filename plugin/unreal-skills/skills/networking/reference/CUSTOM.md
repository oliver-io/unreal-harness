# Custom Replication — The Escape Hatch, and Two Worked Examples

> Status: Reference. When and how to step outside ordinary replication, with two real, neutrally
> documented case studies. Code-free and conceptual. System/device choice is in `REPLICATION.md`;
> the authority discipline that still applies is in `AUTHORITY.md`.

Custom replication is the right tool roughly as often as a custom memory allocator is: occasionally
essential, usually a mistake, and almost never the thing to build *first*. The discipline is to
**default to ordinary replication (Iris or legacy) for everything, and add bespoke machinery only at
the specific points where a *named, measured* engine limit blocks the ordinary path** — not because
custom feels more "serious." This document gives the test for "is this one of those points," what
custom typically means, its real costs, and two worked examples: a team that went custom
surgically because the engine lacked features (§3), and one that fell back to manual replication
because a sanctioned seam demonstrably failed in its configuration (§4).

---

## 1. The test: is custom actually warranted?

Reach for a bespoke mechanism only when you can name the engine limit it works around. Legitimate
triggers:

| Trigger | Why ordinary replication can't | Typical custom response |
|---|---|---|
| **No built-in feature exists** (lag compensation / server-side rewind) | The engine ships nothing; every competitive shooter writes it. | Server-side history ring + rewind/restore (see `AUTHORITY.md` §5). |
| **The payload exceeds an engine channel's capacity** (many per-tick prediction fields beyond the movement component's tiny spare bit budget) | The stock compressed-flags channel is a handful of bits. | A custom quantized move payload riding the *existing* movement RPC seam. |
| **The data isn't net-addressable** (a pointer to a transient/runtime-created object) | The net GUID cache can't reference objects in the transient package — a replicated pointer arrives null. | Serialize **by value** (a stable identifier) and reconstruct the local reference from a deterministic registry both sides build. |
| **You must exceed `NetUpdateFrequency`** (sub-frame remote aim during fast flicks, where the rate ceiling itself is the problem) | Property replication's sampling rate *is* the limit; raising it isn't the fix. | A motion-gated, quantized snapshot channel over an unreliable multicast, with client-side time estimation and interpolation. |
| **Scale/persistence beyond a single authoritative world** (MMO meshing/sharding, cross-shard interest management, backend-streamed economy) | One `UNetDriver` owns one world; the engine doesn't model server-to-server interest. | A backend authority boundary and/or a bespoke transport — usually *around* UE, not replacing its replication. |
| **A sanctioned engine seam demonstrably malfunctions in your configuration** (e.g. the movement component's packed move stream never reaching a real dedicated server under an opt-in backend) | The ordinary path exists but provably doesn't function — verified by a two-client dedicated-server test, not a PIE hunch. | Fall back to *manual* replication of that one actor's state: server-published replicated transform + an input RPC + a hand-rolled reconcile, scoped to the affected pawn (worked example in §4). |

If you can't name which of these applies, the answer is **ordinary replication done well** (push
model, conditions, dormancy, the right device per `REPLICATION.md`), not custom.

Equally important — the **anti-triggers**, the reasons that feel compelling but aren't:

- *"Custom will be more efficient."* Iris + push model + filtering is efficient; profile before
  assuming you'll beat it.
- *"The state is important, so it deserves a custom path."* Importance is an authority/validation
  concern (`AUTHORITY.md`), not a transport one.
- *"We'll need it at scale eventually."* Build for scale *structurally* (adopt Iris, push model,
  dormancy, tiered rates now) but **don't pre-build custom netcode against a load you don't yet
  have.** The discipline: *accept temporary inefficiency, reject temporary lock-in.* Custom
  mechanisms are lock-in; defer each one behind a measured trigger.

---

## 2. What "custom" means, and what it costs

Custom spans a spectrum from "still inside the engine's seams" to "outside the engine entirely":

- **Inside the seams (preferred):** Fast Array serialization for efficient array deltas; a custom
  `NetSerialize`/`NetDeltaSerialize` for a compact wire format; a custom payload riding an existing
  engine RPC. You keep most engine tooling.
- **Outside the engine (last resort):** a dedicated state-replication service, bespoke UDP/WebSocket
  channels, your own snapshot interpolation and reconciliation, a backend database/sim as the real
  authority with UE as an edge/presentation node.

The costs you take on, in full, the moment you leave the ordinary path:

- **You lose engine tooling** — built-in prediction/reconciliation, the network profiler,
  replication visualizers, automatic dormancy/relevancy, and the scaling work in Iris/Replication
  Graph.
- **You own correctness and security end-to-end** — prediction and reconciliation are notoriously
  hard to get right, and every byte you hand-serialize is an anti-cheat surface.
- **You own it forever** — engine upgrades, edge cases, and debugging are now yours.

The default position therefore: **exhaust Iris (with filtering/prioritization) and a clean backend
boundary before writing a replication stack.** When you do go custom, keep it *surgical* — one
mechanism, one named trigger, riding engine seams where possible.

---

## 3. Worked example — surgical custom over an Iris baseline

**A real competitive-FPS-meets-persistent-world project** is a clean illustration of the
discipline, precisely because it is **not** a custom replication system. It is **Iris as the live
backend, push-model property replication and RPCs for the overwhelming majority of state, and exactly
three bespoke pieces** — each pinned to a named engine limit. The stated principle: *"do not replicate
everything one way; pick the cheapest correct device per kind of state,"* and *"keep deep custom
control of movement, adopt Iris for everything else."*

What it left entirely ordinary (the ~90%): replicated properties (push-model, with conditions and
dormancy) for gameplay state; RPCs for discrete combat events; Gameplay Ability System replicating
over Iris; and — notably — the held weapon's transform is **not replicated at all**, because it
follows a replicated, identically-animated bone for free (the best replication is none).

The three bespoke pieces, each with its trigger:

| Custom piece | Named engine limit it works around | Shape |
|---|---|---|
| **Predicted movement payload** | The movement component's spare flag bits can't carry ~21 per-tick prediction fields for deterministic CS-style movement replay. | A quantized bitstream packed into the *existing* server-move RPC (a sanctioned engine extension point, not a forked net stack); conditional field groups so inactive systems cost zero bytes; quantize-before-predict for byte-identical replay. Iris never touches it. |
| **By-value item serialization** | Item definitions are transient runtime objects → **not net-addressable**; a replicated pointer arrives null. | A custom `NetSerialize` sends a stable identifier plus per-instance state; the receiver reconstructs the local pointer from a deterministic registry both sides populate. Net-addressable references still use the ordinary path. |
| **Proxy-motion snapshot channel + lag-comp rewind** | Remote aim during fast flicks must **exceed `NetUpdateFrequency`**, which property replication can't; and the engine ships **no lag compensation**. | A motion-gated (hysteresis on/off), quantized snapshot stream over unreliable multicast — zero bandwidth when slow — reconstructed client-side via a phase-locked server-time estimate and shortest-path interpolation, cross-blending back to the stock proxy path at the boundaries; plus a server-side per-player transform ring for rewind, bounded by server-measured RTT. |

The instructive judgment call: of these, only **lag compensation** and **CS-grade movement
prediction** are *truly* unavoidable (the engine has no equivalent). The by-value serialization is
forced by a hard limitation. The snapshot channel is the most *discretionary* — a feel/polish system
that *supplements* the ordinary proxy path rather than replacing it. **A team that didn't need
favor-the-shooter hit registration or CS-movement feel would need none of these and could ship on
Iris + push model + RPCs alone** — which is exactly the recommended default.

Two more borrowable principles from how it's framed:

- **"Build for the ceiling, ship at the floor."** Adopt the scalable substrate now (Iris, push
  model, dormancy, tiered rates), but explicitly **defer** spatial interest tiering, proxy-AI
  optimization, and zone sharding behind measured triggers — they're documented as *not to be built*
  until profiling demands them.
- **The authority discipline is uniform** regardless of how exotic the transport is: every mutation
  routes through the `Server_` → `_Authoritative` contract, lag-comp is bounded by server time not
  client claims, and identifiers (not pointers) cross the wire. The custom transport changes *how
  bytes move*, never *who is trusted* — see `AUTHORITY.md`.

---

## 4. Second worked example — when the sanctioned seam itself fails

The first example goes custom because the engine *lacks* a feature. The rarer, more jarring shape:
**the sanctioned seam exists but demonstrably malfunctions in your configuration** — and the right
move is a *narrower*, more manual fallback, not a wider custom system.

A small 1v1 vehicle game (UE 5.7, Iris enabled — itself a system choice `REPLICATION.md` Part 1
would counsel against at that scale, which is part of the lesson; dedicated server on a cloud
fleet) built the textbook stack: `ACharacter` + a custom `CharacterMovementComponent`, a deterministic custom-mode
integrator, saved moves, a quantized move payload on the `ServerMove` seam — exactly the §3
pattern. On a real dedicated server the packed move stream **never arrived**; the server pawn froze
at spawn. Earlier in the same effort, engine movement replication had frozen the autonomous proxy
the same way, and a lifetime-condition override on `ReplicatedMovement` had silently done nothing
under Iris (`REPLICATION.md` Part 1, field report). Three correct diagnoses, three textbook fixes
that didn't function.

The recovery, scoped to the two actors whose seams failed (the player pawn and one physics ball):

- **Manual transform replication.** `SetReplicateMovement(false)`; the server integrates movement
  in its own tick and publishes a plain replicated location + yaw; simulated proxies interpolate
  those samples with a short render-in-the-past delay (~80ms), gated on having received a real
  first pose.
- **Input over an unreliable, redundant, sequence-tagged Server RPC.** Each frame the owner sends
  the last N samples; the server applies only samples newer than the last-applied sequence. Loss
  self-heals on the next batch; reordering and duplication are harmless by construction.
- **Hand-rolled owner prediction + anisotropic reconcile.** The owner predicts with the *same*
  deterministic integrator the server runs (fixed sub-stepping so both sides integrate
  identically across frame rates); corrections deadband the legitimate along-track prediction lead,
  promptly correct lateral/vertical error, and ease a presentation transform — never the camera's
  root — over ~100ms. The reconcile math lives in pure, engine-free, unit-tested functions.
- **Everything else stayed ordinary.** Plain replicated properties and RepNotifies for flags and
  match state, reliable Server RPCs for discrete validated events, unreliable multicasts for
  cosmetics. The deterministic integrator was *kept*, so the idle saved-move machinery can be
  re-seated later without redesign.

What it confirms: the fallback preserved the authority model unchanged (server integrates, client
requests, input sanitized server-side); it was surgical — one mechanism per failed seam, ordinary
replication everywhere else; and the failure was only ever visible on a **two-client
dedicated-server run** — the configuration a listen host cannot exercise. The planning corollary:
**prove the engine seam you intend to ride on a real dedicated server before building the
architecture on top of it.**

---

## 5. If you must go custom — a short discipline

1. **Name the trigger** from §1. If you can't, stop and do ordinary replication well.
2. **Ride an engine seam** before inventing a transport (custom payload on an existing RPC, custom
   `NetSerialize` on a struct) — you keep tooling and relevancy. But **prove the seam functions in
   your configuration first** (real dedicated server, two clients); a seam that malfunctions under
   your backend is itself a named trigger for a narrower manual path (§4).
3. **Keep it surgical** — one mechanism per trigger; supplement the ordinary path, don't replace it
   wholesale.
4. **Preserve the authority model** unchanged (`AUTHORITY.md`): server-authoritative, validated,
   bounded by server-measured time, identifiers over pointers.
5. **Quantize and gate** — send the minimum, and only when it matters (motion gates, conditions,
   dormancy) so the custom path costs nothing at rest.
6. **Defer the rest** behind measured triggers; document what you deliberately did *not* build and
   why, so the absence reads as a decision, not an oversight.

---

## Where to go next

- **The ordinary path you should default to** → `REPLICATION.md`.
- **The authority/validation discipline that holds even under custom transports** → `AUTHORITY.md`.
