# Replication — Choosing the System and the Device

> Status: Reference. The core "how should networking be done?" document. Code-free and
> conceptual, but deliberately specific about engine primitives by name. Authority/prediction
> discipline is in `AUTHORITY.md`; the custom escape hatch and two real worked examples are in
> `CUSTOM.md`. This is about state sync *inside* a session; for *where servers run and how
> players are matched onto them*, see the `gamelift` skill.

Most networking confusion comes from collapsing two decisions that are actually independent.
Separate them and the whole design clarifies:

- **Decision 1 — the replication *system* (the backend).** Project/connection-wide. Legacy
  actor replication, Replication Graph, or **Iris**. You pick one.
- **Decision 2 — the *device* per kind of state.** Per-property. A replicated property, an RPC,
  custom serialization, or *nothing at all*. This choice is made the **same way under any
  backend** — Iris runs ordinary replicated properties and RPCs.

The headline guidance: **default to Iris as the system, then choose the cheapest correct device
for each kind of state — and reach past ordinary out-of-the-box replication only where the engine
genuinely can't express what you need.** "Iris for everything" does not mean "exotic machinery for
everything"; it means *run the modern backend, and most of your state is still plain replicated
properties and RPCs.*

---

## Part 1 — Choosing the replication system

### The three systems

| System | What it is | When it's right | Maturity (UE 5.7) |
|---|---|---|---|
| **Legacy actor replication** | The classic `UNetDriver` + actor-channel model: the server compares each replicated property to its last-sent shadow value every tick and ships the deltas. | Small/medium connection and actor counts; mostly low-frequency or event-driven state; you want the most stable, best-tooled, best-documented path. | The engine **default**. Battle-tested. |
| **Replication Graph** | A *replication driver* that replaces only the **relevancy/gather stage** — persistent nodes build and cache per-connection actor lists instead of every actor re-evaluating every connection each frame. Properties/RPCs/RepNotify are unchanged. | You stay on legacy **and** need large-scale relevancy (Fortnite-class: ~100 players, tens of thousands of actors). | Mature but legacy-leaning. Splitscreen (multi-connection-per-client) and replay/demo caveats — verify for your targets. **Mutually exclusive with Iris.** |
| **Iris** | A ground-up **replacement for the core replication system**. Keeps a quantized copy of replicated state, shares expensive work once across connections, moves relevancy into built-in **filtering + prioritization**, and is built for high object/connection counts and parallelism. | The recommended **default for new realtime games of meaningful scale** going forward — frequently-changing, high-object-count gameplay. | **Opt-in** (legacy is still the default even though Iris compiles in). Status is the load-bearing caveat below. |

### The honest Iris maturity caveat

State this plainly when advising — do not oversell it:

- Epic's **Iris documentation banner in 5.7 still says "Experimental — use caution when
  shipping."** The **public roadmap** carries an "Iris (Beta)" card and community sources report a
  promotion to Beta in 5.7. Treat Iris as **"Beta-grade in practice, Experimental-labeled in
  docs"** and **verify the banner on your exact engine build.**
- Iris's production pedigree is **Fortnite** (ported in 2022, hardened since); broadly-named other
  shipped titles are unverified. Real, but not yet ubiquitous.
- Adopting Iris means **budgeting to re-test the full networked feature set** and **pinning/tracking
  the engine version** — Experimental/Beta status permits breaking internal changes between point
  releases.

### When to choose which system

1. **Small, simple, or low-frequency game, small player counts?** Legacy is the right call. Iris's
   payoff grows with object and connection count; at small actor counts the migration cost usually
   isn't repaid, and at moderate player counts community benchmarks report the two performing
   comparably — profile your own workload rather than trusting a threshold. Don't adopt Iris as a
   cargo cult — adopt it for scale.
2. **Building a new realtime game you expect to scale (many players, many interactive objects,
   continuous state)?** Default to **Iris**, with the caveats above. It is where Epic is investing
   and where the relevancy/CPU scaling story lives.
3. **On legacy today and hitting a server-CPU relevancy wall at large scale?** Reach for
   **Replication Graph** — *unless* you're moving to Iris, in which case Iris's own filtering
   replaces it (you can't run both).

### What adopting Iris changes for the engineer

Mostly nothing — that's the point. The migration is "minor modification," not a rewrite:

| Stays the same under Iris | Needs attention under Iris |
|---|---|
| Replicated `UPROPERTY`s and `OnRep_`/RepNotify | **Subobject replication** must use the registered subobject list (`bReplicateUsingRegisteredSubObjectList`); custom non-actor subobjects register replication fragments |
| RPCs (Server/Client/NetMulticast, reliable/unreliable) | **Custom `NetSerialize` structs** must be registered in config so Iris doesn't diverge from your hand-written wire format |
| Dormancy, net conditions, Fast Array replication — but *verify* condition **overrides** on engine built-in properties actually take effect (see the field report below) | **Relevancy moves** from per-actor flags + Replication Graph to Iris **filtering** (direct, or **NetObjectGroups** for many objects) + **prioritization** (a per-object float; not everything fits each frame) |
| The push model (still optional — Iris falls back to polling) | Re-test **RPC ordering, subobjects, late-join, and the whole movement pipeline** — the edge cases where behavior can differ |

**Field report (UE 5.7, Iris enabled, real dedicated server):** a shipped small-scale project hit
three real divergences squarely inside that "needs attention" zone — each after a *correct
diagnosis* and a textbook fix that silently did nothing:

- A **lifetime-condition override on an engine built-in property** (relaxing `ReplicatedMovement`
  from `COND_SimulatedOrPhysics` to `COND_None` in `GetLifetimeReplicatedProps`) did not take
  effect under Iris — the autonomous proxy still never received the property.
- **Engine movement replication left the autonomous proxy frozen at spawn** on the dedicated
  server; engine physics-body replication for a simulated rigid body misbehaved the same way.
- The **CharacterMovementComponent's packed `ServerMove` stream never arrived** at the dedicated
  server, freezing the server-side pawn and everything downstream of it.

The working recovery in every case was **manual replication** — `SetReplicateMovement(false)` plus
a plain replicated transform published by the server each tick, with input carried by an RPC (see
the device table below and `CUSTOM.md` §4). The lesson is not "Iris breaks movement"; it's that
**movement, built-in-property condition overrides, and the CMC move stream are exactly where Iris
behavior can differ — run the two-client dedicated-server smoke on your movement pipeline in week
one, before building the architecture on top of it.**

---

## Part 2 — Choosing the device per kind of state

This is the decision you make over and over, regardless of backend. The governing question is the
**change-frequency × who-needs-it axis.**

### The device table

| Kind of state | Device | Why |
|---|---|---|
| **Changes rarely / occasionally; needs to be correct for late joiners** (health, team, config, ownership, equipped item) | **Replicated property** (push-model), often with **dormancy** and a **`COND_*` condition** | Replication is stateful — it reconciles late joiners for free. Dormancy means the server stops considering an idle actor each tick. Conditions stop sending it to connections that don't need it. This is the OOTB sweet spot — **don't over-engineer it.** |
| **Changes every tick; continuous** (the local player's predicted movement, a charging meter, a streamed value) | **Push-model replicated property** — or, for the owning player's physics, a custom move payload (see `CUSTOM.md`) | Continuous state wants the steady property path; push model keeps it cheap by shipping only when marked dirty. |
| **The owning client's continuous input stream** (per-tick throttle/steer/aim — when it isn't riding the movement component's own move stream) | **Unreliable Server RPC carrying a redundant window of the last N sequence-tagged samples** | Per-tick reliables risk reliable-buffer overflow; a lone unreliable sample dies with its packet. Sending the last few samples every frame makes loss self-healing, and a server that applies only samples newer than the last-applied sequence makes reordering and duplication harmless. |
| **A discrete, infrequent event** (a hit, an ability fire, a door open, a one-shot effect trigger) | **RPC** (Server for client→server requests, NetMulticast/Client for server→clients) | Events are moments, not state. *"Late-joiner bookkeeping is a one-time cost; polling an event-shaped value as a property is paid every tick forever."* Prefer an RPC for things that happen, a property for things that *are*. |
| **Cosmetic / presentation-only** (muzzle flash, footstep dust, UI flourish) | **Replicate the *trigger*, play the cosmetic locally** | Never replicate the visual itself. Replicate the gameplay cause; let each client render the effect. |
| **Already derivable from other replicated state** (a held weapon's transform when it's attached to a replicated, identically-animated bone) | **Nothing** | The best replication is no replication. If a value follows from something already replicated, derive it locally instead of streaming it. |
| **High-rate state that must exceed `NetUpdateFrequency`** (sub-frame remote aim during fast flicks) or **state the engine can't address** (a pointer to a transient object) | **Custom serialization / a bespoke channel** — the escape hatch | Only when a measured engine limit is in the way. See `CUSTOM.md`. |

### The one-line rule

> Continuous *state* → push-model property. Discrete *event* → RPC. The local player's *physics* →
> a predicted move payload. A value that *follows from* already-replicated state → nothing. Reach
> for custom serialization only when a named engine limit blocks all of the above. **When genuinely
> unsure between a property and an RPC, prefer the RPC** — its cost is a one-time "send current
> state to late joiners" hook, not a per-tick tax.

### The push model — the high-ROI *scale* lever

Independent of backend or device, **turn on the push model once actor/connection count makes the
per-tick diff measurable** (`net.IsPushModelEnabled=1`; mark properties push-based; call
`MARK_PROPERTY_DIRTY` at *every* mutation site). It converts the server's per-tick "diff every
property of every actor" into "ship only what was marked dirty" — on actor-heavy scenes this is
the single biggest server-CPU win, and it's the substrate Iris prefers.

**The one footgun:** a missed dirty-mark fails **silently** — no error, the client simply never
sees the change. Every write to a push-model property must be paired with its dirty-mark.

**It's a scale lever, not a correctness requirement.** A tiny session (a 1v1 with a handful of
replicated actors) gains almost nothing from it, while every property write acquires the
silent-footgun liability — plain `DOREPLIFETIME` is a perfectly sound shipping choice at that
scale. Adopt push model when actor/connection count makes the per-tick diff measurable.

### Bandwidth and relevancy hygiene (reach for these before a bigger system)

`NetUpdateFrequency` / `MinNetUpdateFrequency` (lower for rarely-changing actors),
`NetCullDistanceSquared` (distance relevancy), dormancy (`DORM_*`), net conditions (`COND_OwnerOnly`,
`COND_SkipOwner`, `COND_SimulatedOnly`, `COND_InitialOnly`), and `NetPriority` often defer the need
for Replication Graph or Iris entirely. Quantize high-frequency floats; never leave update frequency
high on cheap actors.

**The inverse hygiene for small sessions:** a game with a handful of must-see actors (a 1v1 arena,
a co-op boss) should simply set `bAlwaysRelevant` on them. Default distance relevancy
(`NetCullDistanceSquared`, ~150m) silently closes the actor channel of anything far away — the
first symptom is "the other player never appears" — and per-actor culling buys nothing when every
client must see every gameplay actor anyway. Culling hygiene is for actor counts worth culling.

---

## Part 3 — Common mistakes

| Mistake | Fix |
|---|---|
| Replicating cosmetic/visual-only state | Replicate the trigger; play the effect locally. |
| Treating an event as a property (polling a value that's really "something happened") | Use an RPC. |
| Reliable-RPC spam for high-rate calls | Reliables are for infrequent, important calls; high-rate/cosmetic calls go unreliable. Reliable-buffer overflow disconnects clients. |
| Leaving `NetUpdateFrequency` high and no dormancy/conditions on rarely-changing actors | Tier the rate, sleep idle actors, scope with `COND_*`. |
| Missed `MARK_PROPERTY_DIRTY` on a push-model property | Pair every write with its dirty-mark; the failure is silent. |
| Re-deriving presentation state client-side from replicated data that resolves null off-server (e.g. a transient item definition) | Publish the presentation result as its own replicated state; don't recompute it where the dependency isn't net-addressable. |
| Plain `Replicated` on state clients must *react* to (death, team, match phase) | Use RepNotify (`ReplicatedUsing`) and drive the reaction from the `OnRep_`. On a listen host the host player never needs the notify, so the omission hides until a dedicated server makes every player remote. |
| Interpolating a manually-replicated transform before the first server sample arrives | The property still holds its zero default, so the proxy visibly pops to the world origin on relevancy-gain. Gate interpolation on "has received a real pose." |
| Adopting Iris for a small/low-frequency game | Iris pays off with scale; a small game is better served by legacy + ordinary replication (push model when actor count warrants it — see the temper above). |
| Verifying multiplayer in single-process PIE | A listen-host has authority, so RPC routing never exercises. Not a multiplayer test — see `AUTHORITY.md`. |

---

## Where to go next

- **Server authority, client prediction, validation, time sync** — the discipline that's true under
  any backend → `AUTHORITY.md`.
- **When and how to go custom, with two real worked examples** → `CUSTOM.md`.
