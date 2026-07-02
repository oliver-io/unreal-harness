# Authority, Prediction, and Time — The Discipline

> Status: Reference. The "how to do networking *properly*" invariants. These are true under **any**
> replication system (legacy, Replication Graph, or Iris) and under **any** device choice. Code-free
> and conceptual. The system/device choices are in `REPLICATION.md`; custom mechanisms in `CUSTOM.md`.

Replication is *how* state moves; authority is *who is allowed to change it.* Getting the device
right but the authority model wrong yields a game that desyncs, rubber-bands, or is trivially
cheatable. This document is the discipline that makes networked state correct. It is mostly
independent of which backend you run.

---

## 1. The model: server-authoritative, client-predicted, favor-the-shooter

The default stance for any competitive or persistent multiplayer game:

- **The server is the sole authority.** Every gameplay-meaningful change is decided on the server.
  A client *requests*; it does not *do*.
- **Clients predict their own controlled pawn** for zero-latency feel — local input drives local
  movement immediately, optimistically.
- **On disagreement, the server's result is truth, and the client corrects smoothly** rather than
  snapping.
- **Hit registration is server-side, lag-compensated** ("favor the shooter") so that what the
  player aimed at when they clicked is what the server tests.

This maps onto UE's network roles: the authoritative copy (`ROLE_Authority`, the server), the
owning client's predicted pawn (`ROLE_AutonomousProxy`), and everyone else's interpolated copies
(`ROLE_SimulatedProxy`). The invocation rule that follows: a **Server RPC invoked from a machine
that doesn't own the actor is dropped**, and a client-invoked multicast runs only locally — only
the authority decides, and only the owning (autonomous) client may request. (Server-*sent*
multicasts do, of course, execute on simulated proxies — that's how cosmetic triggers reach remote
clients.)

---

## 2. The mutation contract (never trust the caller's context)

The most common dedicated-server bug: code written single-player or listen-host "just worked"
because the caller always had authority. On a dedicated server, a remote client calling that same
mutator changes only its *local* mirror — and may trigger backend side effects from an untrusted
context. The fix is a uniform contract for **every authoritative mutation:**

> **public method → routes on authority → `Server_` RPC (off authority) → `_Authoritative` body
> (on authority only).**

- The public entry point checks authority. On the authority it runs the real work and returns the
  real result; off the authority it fires the `Server_` request RPC and returns "request issued."
- The local mirror updates **only via replication after the server accepts** — never optimistically
  for authoritative state (movement prediction is the sanctioned exception, §3).
- **Backend / external side effects (HTTP, economy, persistence) fire only from authority** — never
  from a remote client's local call.

This is a *discipline*, not a mechanism — it works identically under legacy or Iris. It is the
single highest-value invariant in a dedicated-server codebase.

---

## 3. Prediction and reconciliation (the one place clients act first)

Movement is the sanctioned exception to "request, don't do," because latency on your own movement
is unacceptable. UE's `UCharacterMovementComponent` ships the full pattern out of the box:

- The owning client records moves into a saved-move buffer, runs the movement locally, and sends
  the moves to the server.
- The server **re-simulates the same physics** and, on divergence beyond tolerance, sends a
  correction; the client **replays** its unacknowledged moves from the correction point.
- Simulated proxies (other players) are **interpolated/smoothed** toward replicated state, not
  predicted.

**Prove the move stream on a real dedicated server before building on it** — especially under an
opt-in backend like Iris. A UE 5.7 field report (Iris enabled, cloud dedicated server) found the
packed `ServerMove` stream simply never arrived at the server — the server-side pawn froze at
spawn, and with it walls, hit detection, and the opponent's pose ever updating. The full saved-move machinery
compiled and worked in PIE; only the two-client dedicated-server run exposed the dead seam. The
working recovery was a manual path — a server-published replicated transform, an unreliable
redundant sequence-tagged input RPC, and a hand-rolled reconcile (`CUSTOM.md` §4). Run that smoke
in week one so a dead seam costs a day, not the architecture.

The invariants that keep prediction honest:

- **Never run the simulated-proxy pattern on the autonomous proxy.** Snapping or interpolating the
  owner's own pawn toward replicated state — with no local prediction — *is* rubber-banding by
  construction: the owner lags a full RTT behind its own input. It's worst when the camera is
  welded to the net-driven root, because then the viewpoint itself judders. The owner predicts and
  is *corrected*; corrections should ease a presentation/cosmetic transform over ~100ms, never
  teleport the component the camera hangs from.
- **Correct anisotropically.** Along the direction of travel the owner *legitimately* leads the
  server by about half an RTT — correcting that lead re-introduces the input latency prediction
  exists to hide, so give along-track error a generous deadband. Lateral and vertical divergence
  is *always* wrong (it's what puts a trail, wall, or aim off to the side) and should be corrected
  promptly with little or no deadband.
- **Never change movement state from an input handler.** Input handlers fire only on the owning
  client — the server never runs them, so the change can't be reconciled and the client
  rubber-bands. Movement state changes belong in the movement pipeline that **both** sides run.
- **Predict-then-validate, don't predict-then-hope.** Where a client predicts an action (e.g. a
  special jump), the server must **re-apply the same math behind cheap validation gates** and reject
  illegal cases — not blindly trust the client, and not re-derive context that may have drifted.
- **Quantize before you predict.** If a predicted value is transmitted quantized, snap it to wire
  precision *before* running the simulation, so client and server compute from byte-identical
  inputs. Otherwise the two diverge by rounding alone.

Forward-looking note: Epic's **Mover (2.0)** plugin is the intended successor to the
`CharacterMovementComponent` with rollback-style networking, but in 5.7 it is experimental — design
on it only with a long runway and an appetite for API churn; otherwise the movement component is the
shipping path today.

---

## 4. Validation and anti-cheat (trust nothing the client asserts)

- **Server RPCs that carry consequence get a validation function** — verify client-supplied
  parameters against game rules; reject (and on egregious violations, disconnect) on failure.
- **Cross-check stale/ambiguous requests.** When a client asks to act on a slot/target it named,
  have it pass what it *thinks* is there and let the server reject if reality has changed (the item
  moved, the target died). This closes desync-driven exploits.
- **Guard authority by entity liveness.** A dead or torn-down pawn must reject client mutations.
- **Bound everything by server-measured quantities, never client-claimed ones** (§5).
- **The self-harm exception.** A client report that can only *disadvantage the reporter* (e.g. "I
  hit the hazard — I'm dead") may be trusted for frame-accurate responsiveness: a cheater gains
  nothing by lying. Keep the server's own detection as the authoritative backstop, and have the
  client un-do the predicted state if no server confirmation arrives within a short timeout.

---

## 5. Lag compensation and time (server time is the only clock that counts)

"Favor the shooter" requires the server to reconstruct *what the shooter saw* — which means rewinding
other entities to a past instant. The engine ships **no** lag compensation; every competitive shooter
builds it. The correct shape:

- The server keeps a short **history ring** of each entity's relevant transforms (e.g. hitbox bones)
  over the last few hundred milliseconds.
- On a shot, it rewinds eligible entities to the shooter's click-time view, tests the hit, and
  restores them.
- **The rewind window is bounded by *server-measured* round-trip time** (the connection's measured
  lag), **never by a timestamp the client supplies.** Future-dated shots are rejected; any
  client-supplied interpolation delay is clamped to a sane maximum; client-supplied aim is trusted
  only within a sanity-angle gate.

**Favor-the-shooter is a stance, not a law.** It's the right default for shooters, where the
attacker's perception must win. Other games legitimately choose **favor-present** — no rewind, the
server's current world is truth — and compensate *narrowly* only where the perception gap
demonstrably hurts (a field example: extending only the reporter's own trailing hazard toward
where they perceived it, capped at a fixed distance — no rewind of anything else). The choice per
interaction is a design decision to make explicitly; what survives every stance is the invariant
that whatever compensation exists is bounded by server-measured quantities, never client claims.
One consequence worth documenting when you choose favor-present: the client renders its own pawn
~½ RTT ahead and remote objects ~½ RTT + interpolation-delay behind, so "it clearly hit me on my
screen" complaints are structural — decide deliberately whether each collision favors the
defender's view or the server's present.

**Time synchronization** underpins all of this. Establish **one definition of server time** — a
monotonic, replicated game-world clock (the GameState's replicated server-world-time) — and have
every timeline (ability windows, session timers, charge meters) reconstruct from it so all machines
agree. High-rate sub-frame systems (e.g. smooth remote-aim reconstruction) may run a finer per-entity
clock estimated from a phase-locked loop over timestamps in the stream, but it should be
sanity-bounded against the coarse replicated clock, not a second source of truth.

---

## 6. Late joiners and the listen-vs-dedicated trap

- **RPCs are moments; they don't replay for late joiners.** Any state delivered by RPC needs a
  manual "send current state" step when a client arrives or possesses a pawn (e.g. in the
  possession/controller-ready hook). State delivered by *replicated property* reconciles late
  joiners automatically — another reason to prefer properties for things that *are* and RPCs for
  things that *happen*.
- **Prefer a dedicated server for competitive/authoritative integrity.** A listen server makes the
  host a player with authority, creating an asymmetry (and an anti-cheat hole). Co-op and small
  sessions can use listen servers; competitive and persistent games should not.
- **Strip client-only components from the dedicated server.** Cameras, spring arms, and other
  presentation-only components waste server frame time if they tick on a headless host.

### Testing invariant

> **Single-process Play-In-Editor is not a multiplayer test.** A listen-host has authority, so the
> `Server_` RPC routing, the autonomous/simulated split, and late-join paths never exercise. A
> networked change is not verified until it runs with a **real dedicated server and at least two
> connected clients.** Treat the two-client run as the acceptance gate for any networking change.

Bug classes that PIE / a listen host reliably masks, from the field:

- **`OnRep_` handlers that never fire for the host's local player** — a plain replicated flag with
  no notify (e.g. `bDead`) "works" on a listen host and silently breaks reactions the moment a
  dedicated server makes every player remote.
- **UI input-mode / focus state across a real travel** — Slate focus and input-mode handovers that
  survive PIE reset across a genuine `ClientTravel`, leaving polled input reading empty on the
  real client only ("input does nothing" on the build, fine in-editor).
- **The movement component's `ServerMove` stream** — a listen host never routes it over a socket,
  so a dead move-stream seam (§3) is invisible until the dedicated run.

---

## 7. The invariant checklist

Carry these into any networking review:

1. Every authoritative mutation goes public-method → `Server_` RPC → `_Authoritative`; never call
   the authoritative body from a remote client.
2. Backend/external side effects fire only from authority.
3. Never change movement state from an input handler; movement changes live in the pipeline both
   sides run.
4. Predicted actions are re-applied and validated server-side, not trusted.
5. Quantize predicted values to wire precision before simulating.
6. Server RPCs with consequence have validation; reject stale/illegal requests.
7. Lag-comp rewind is bounded by server-measured RTT, never client timestamps; aim trusted only
   within a sanity gate.
8. One replicated definition of server time; all timelines derive from it.
9. RPC-delivered state has an explicit late-joiner "send current state" path.
10. Prefer dedicated servers; strip client-only components from them.
11. A networking change isn't done until the two-client dedicated-server smoke passes.
12. The autonomous proxy predicts — it is never snapped/interpolated to replicated state; corrections
    ease a presentation transform (deadband the along-track lead, correct lateral error promptly)
    and never teleport the component the camera hangs from.
13. Engine seams the architecture depends on (the move stream, condition overrides) are proven on a
    real dedicated server *before* building on them.

---

## Where to go next

- **Which system and device to use for each kind of state** → `REPLICATION.md`.
- **When the engine genuinely can't express what you need — or a sanctioned seam fails — and real
  examples of handling both surgically** → `CUSTOM.md`.
