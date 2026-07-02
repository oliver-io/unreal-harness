# Integration Patterns — How Much of GameLift to Own

> Status: Reference. Maps the ownership spectrum and documents two real reference integrations
> neutrally. Code-free and conceptual. Platform facts in `PLATFORM.md`; the placement spine in
> `QUEUES.md`.

Every GameLift integration is a position on a single spectrum: **how much of the
match-place-run-recycle lifecycle does AWS own, versus how much do you build?** There is no
"correct" position — the right one is a function of your game's shape, your team's appetite for
operating infrastructure, and how far your needs diverge from GameLift's managed defaults. This
document gives the spectrum, then plots two real integrations and the canonical industry patterns
onto it, so an advisor can say "you are here, and the tradeoffs are these."

---

## 1. The ownership spectrum

```
 DELEGATE ◀──────────────────────────────────────────────────────────▶ OWN
 everything managed                                          everything custom

 FlexMatch          Custom matcher        Custom control plane      FleetIQ /
 WITH_QUEUE    ▸    + GameLift queue   ▸   + GameLift compute   ▸    self-managed
 + managed fleets       /placement          (queue as a pipe)        orchestration
 ───────────────       ──────────────       ────────────────────     ──────────────
 least code            some code            substantial code         most code
 least control         some control         high control            total control
 fastest to ship       moderate             slow to build           slowest
```

Five capabilities can each independently be GameLift's or yours. Where each falls is the whole
design:

| Capability | Delegate to GameLift | Own it yourself |
|---|---|---|
| **Matchmaking** (who plays together) | FlexMatch rule sets | Your own matcher (cohorts, parties, skill, FIFO-by-N) |
| **Placement** (which host) | GameSessionQueue priority/latency/cost | Your own host selection + `CreateGameSession` |
| **Capacity / scaling** | Target-based auto-scaling | Drive `UpdateFleetCapacity` from your own demand signal |
| **Occupancy / reservations** | PlayerSession accounting | Your own reservation ledger + overfill guard |
| **Compute orchestration** | Managed EC2/container fleets | FleetIQ GameServerGroups / your own EC2/ECS/EKS |

The further right you sit on any row, the more code and operational burden you take on — and the
more the behavior is exactly what you want rather than what GameLift offers.

---

## 2. Reference integration A — the AWS-native pattern (lean delegation)

**One real example: the HOVERBALL service layer** (the 1v1 sample game in this repo, at
`projects/hoverball/service/`). It is a deliberate "let GameLift do as much as possible" integration.

> **Status note:** HOVERBALL's service layer is the *provisioning + matchmaking* half of online
> multiplayer and is largely codified; the in-engine replication it will eventually host is
> **deferred/unbuilt** per the game's own status docs. The pattern below is what the stack does,
> not a claim that the game ships networked play today.

**The shape.** A small Pulumi (TypeScript) stack provisions, at deploy time: a **managed-EC2
fleet** (on-demand, scale-from-zero, several `ConcurrentExecutions` per host), an **alias**, a
**GameSessionQueue** with that alias as its one destination, and a **FlexMatch** matchmaker
(`WITH_QUEUE`) with a **count-only rule set** (two players, one team). The *only* bespoke runtime
component is a thin, stateless **Lambda** behind a public URL exposing two verbs — `start`
(→`StartMatchmaking`) and `poll` (→`DescribeMatchmaking`, returning IP/port/player-session on
COMPLETED).

**Where it sits on the spectrum:** far left on every row.

| Capability | HOVERBALL |
|---|---|
| Matchmaking | **GameLift** (FlexMatch count-only rule set) |
| Placement | **GameLift** (queue, WITH_QUEUE — never calls placement itself) |
| Capacity | **GameLift** (native target-tracking, scale-from-zero) |
| Occupancy | **GameLift** (PlayerSession from the completed ticket) |
| Compute | **GameLift** (managed EC2) |

**Why this fits the game.** The matchmaking requirement is literally "pair the next two players" —
a count-only rule expresses it exactly, and skill/latency/backfill/acceptance would be unused
machinery. The stateful part of the request path is FlexMatch itself; the team operates **~70 lines
of stateless glue** and a declarative stack, with no datastore, no session registry, no capacity
controller. Scale-from-zero means no idle cost when nobody plays.

**What you give up.** You accept GameLift's session-oriented model and its placement/matchmaking
semantics wholesale. There is little room to express custom grouping, custom host selection, or
your own reservation guarantees. For a small session-based game, that's the *point* — the
constraints aren't binding.

**This maps to `QUEUES.md` topology "FlexMatch WITH_QUEUE,"** the canonical "let GameLift do
everything" wiring, plus serverless glue (Lambda) in front.

**Real-world build notes** (what actually shipping this taught, beyond the conceptual shape):
- **IaC = classic `@pulumi/aws`.** Two provider potholes had to be worked around (see `PLATFORM.md`
  §8/§10): its `aws.gamelift.Build` can't set `server_sdk_version`, so the AL2023 build is created
  via `aws gamelift upload-build --server-sdk-version 5.x` and fed to the fleet as an external build
  ID; and its FlexMatch resources are deprecation-flagged (still functional).
- **Scale-from-zero is the deliberate default** (`min 0` — no idle spend; the first entrant warms
  the fleet, and a cold first boot is accepted). Because target-tracking won't wake an instance from
  *absolute* zero, the matchmake Lambda nudges `DESIRED` 0→1 on the first ticket, and the match/queue
  timeouts are long enough (~300s) to survive the boot; scale-DOWN back to 0 is target-tracking. Warm
  headroom (`min 1`) is an opt-in config change for when traffic warrants — not the default.
- **WITH_QUEUE means no `CreatePlayerSession` call** — the completed matchmaking ticket already
  carries each player's `PlayerSessionId`; the Lambda just reads it. (A common over-build is to
  re-create player sessions the matchmaker already made.)
- **Validated locally without AWS**: an in-memory matcher mirror reproduces the pull-by-twos
  contract, and a stub matchmaker spawns the real *cooked* dedicated server per pair so two clients
  land in one session. The dedicated server must be **cooked/staged** to run (an uncooked
  `Binaries/` run crashes in async loading — `PLATFORM.md` §10).

---

## 3. Reference integration B — the fully-owned control plane (deep ownership)

**One real example** — a persistent multiplayer game (a production reference project). It is a
deliberate "GameLift is a replaceable execution backend; we own the orchestration" integration,
driven by needs a session-based managed path doesn't serve well.

**The shape.** A long-running, event-sourced **control plane** (Bun/TypeScript on ECS Fargate)
holds authoritative in-memory state over a `Family → Cluster → Shard` hierarchy and owns matching,
placement, capacity, and reservations itself. GameLift is reached only through a launcher
abstraction (`ShardLauncher`) — the same orchestration logic drives a test stub, locally-spawned
dedicated-server processes, or GameLift with no change to control-plane code. On the GameLift side
it uses **Build + Fleet + Alias + a single-destination Queue** and the raw **placement / session /
capacity APIs** — and bypasses GameLift's higher-level orchestration.

**Where it sits on the spectrum:** far right on the orchestration rows, while still using GameLift
*compute*.

| Capability | Owned control plane | Note |
|---|---|---|
| Matchmaking | **Own** | A custom cohort matcher (fill-to-size or age-out with bot fill). No FlexMatch. |
| Placement | **Own** | The control plane selects the shard; the queue is a one-destination pipe reached *after* that decision. |
| Capacity | **Own** | The launcher drives `UpdateFleetCapacity` from the control plane's own count of pending+active work (scale-from-zero). GameLift auto-scaling is off. |
| Occupancy | **Own** | A custom reservation manager (timed reservations, overfill guard) — independent of GameLift PlayerSession accounting. |
| Compute | **GameLift** | Managed-EC2 fleets, one process per host, one session per process. |

**Why this design.** The headline payoff is a **single seam**: because orchestration is owned and
GameLift sits behind an interface, the *identical* logic runs as a deterministic test stub, against
local `Dedicated.exe` for development, or against GameLift in production — GameLift is not
load-bearing for dev or test. Event-sourcing yields replay and observability for free. Owning
matching/placement/reservations lets the team express semantics GameLift's managed features don't
(cohorting with bot-fill, reservations with a last-slot overfill guard, retry-through-cold-boot)
and a persistent-world shape the session model resists.

**Why specific GameLift features are *not* used** (instructive for any control-plane builder):

- **FlexMatch** — the grouping needed (cohort fill-or-timeout, bot fill) isn't skill/team/party
  matching, so a minimal custom matcher fit better than adopting FlexMatch.
- **GameLift auto-scaling** — the team must count the "placed but still booting" window itself to
  avoid scaling a host out from under a server mid-initialization, so capacity is driven from their
  own in-memory demand signal rather than GameLift's session metric.
- **Queue placement intelligence** — role isolation (e.g. social-hub vs match-instance) on
  one-process-per-host fleets is the real requirement; a single shared multi-destination queue
  actually *deadlocked* (a persistent session held the only process), which forced one
  fleet+queue per role with single-destination queues.
- **PlayerSession reservations** — replaced by a custom reservation ledger to close a
  double-placement race on the last slot, which GameLift's accounting didn't express the way they
  needed.

**What you take on** (honest costs, from the project's own notes): authoritative in-memory state is
lost on a backend redeploy unless you build rehydration (they haven't yet); the control plane's
state can outlive the GameLift session it points at, needing a custom liveness/self-heal gate;
imperative fleet creation alongside declarative queues creates ordering/teardown seams. This is the
real price of deep ownership.

**This maps to `QUEUES.md` topology "Custom matcher + direct/owned placement"** — GameLift as
managed compute under a custom control plane, with the queue degraded to a pipe.

---

## 4. The two reference modes side by side

| Dimension | AWS-native | Owned control plane |
|---|---|---|
| Game shape | Small, session-based, fixed 1v1 | Persistent, multi-role, many concurrent shards |
| Matchmaking | FlexMatch (count-only) | Custom cohort matcher |
| Placement | GameLift queue (WITH_QUEUE) | Control plane; queue is a pipe |
| Capacity | GameLift target-tracking | Launcher-driven from owned demand signal |
| Reservations | GameLift PlayerSessions | Custom reservation ledger |
| Compute | Managed EC2 | Managed EC2 |
| Bespoke runtime | ~70-line stateless Lambda | Long-running event-sourced service on ECS |
| Local/dev story | In-memory matcher mirror + stub matchmaker spawning the real cooked server (Docker for the Linux path) | One launcher seam: stub / local process / GameLift |
| Operational burden | Minimal | Substantial (state, liveness, deploy ordering) |
| Best when | Needs are within GameLift's defaults | Needs diverge from the session model / want full control + a portable seam |

Both choose **managed EC2** for compute — the divergence is entirely in the orchestration rows.
That's the useful lesson: **the hosting flavor and the ownership level are independent decisions.**
You can run maximally-managed compute under a maximally-custom control plane, or vice versa.

---

## 5. The canonical industry patterns (for completeness)

Beyond our two, these are the patterns AWS's own reference material describes. An advisor should
recognize all of them:

- **"Let GameLift do everything."** FlexMatch `WITH_QUEUE` + managed multi-region fleets +
  auto-scaling, minimal backend. Fastest to ship; least control. (HOVERBALL is this, scaled down.)
- **Custom matchmaker / custom control plane.** GameLift for compute; you own matching and/or
  placement (`CreateGameSession` directly, or FlexMatch `STANDALONE`). High control. (The
  owned-control-plane reference is this.)
- **FleetIQ + GameServerGroups.** GameLift manages a Spot Auto Scaling group **in your account**
  for viability and ~90% Spot savings, but you self-orchestrate placement/matching
  (`ClaimGameServer`) and integrate your own ECS/EKS/Agones. No queues. Max control + cheapest Spot;
  most glue.
- **Serverless backend glue (the canonical reference).** Client → Cognito → API Gateway → Lambda →
  GameLift; placement/matchmaking events → SNS/EventBridge → Lambda → DynamoDB; client polls your
  API for the resulting connection info. This is *plumbing*, composable with any of the above — and
  is exactly the shape of HOVERBALL's Lambda, just fuller.
- **Session-based vs persistent-world / MMO.** GameLift is session-oriented (one process, one
  session, expected to end). Persistent worlds adapt with long-lived sessions and/or world
  sharding — which fights the model and is a primary reason a team builds a control plane.
  ⚠️ Long-lived sessions also undermine FleetIQ Spot balancing (AWS recommends Spot for sessions
  under ~2 hours), so persistent worlds lean On-Demand or self-managed compute.

---

## 6. Choosing a position — an advisor's questions

Ask these to locate someone on the spectrum. Lean **left (delegate)** until a concrete need pushes
right; every step right is real, ongoing operational cost.

1. **Is your game session-based with a clear end, or a persistent world?** Persistent worlds push
   hard right (custom control plane) because the session model resists them.
2. **Does your grouping fit a rule set** (counts, teams, skill, latency, parties)? If yes, FlexMatch
   delegates it. If your grouping is genuinely bespoke (cohorts, bot-fill, your own skill system),
   own the matcher.
3. **Do you need placement semantics GameLift's queue doesn't express** (role isolation on
   one-process hosts, your own host selection, your own reservation guarantees)? If not, use the
   queue. If yes, own placement and/or capacity.
4. **What's your appetite for operating infrastructure?** A custom control plane is a long-running,
   stateful service you must keep alive, rehydrate, observe, and reconcile. If that's not where your
   team wants to spend, stay left.
5. **Do you need a portable seam** (run the same orchestration locally / in tests / in prod without
   GameLift being load-bearing)? That portability is a real reason to own orchestration behind an
   interface — but you can also get most of it from Anywhere fleets for local iteration without
   building a control plane.
6. **Cost posture?** Scale-from-zero (managed auto-scaling or owned) saves idle cost at the price of
   cold-start latency; Spot saves more but needs queues + graceful interruption handling and suits
   short sessions.

**House default for this org: scale smoothly from zero unless the user asks otherwise.** Fleets ship
`min 0`/`desired 0` so there is **no AWS spend without traffic**, and the **first entrant to the queue
warms it up** (a cold first boot is an accepted tradeoff). We **deploy to AWS early for testers** with
this cheap scale-from-zero posture rather than over-investing in local emulation — local harnesses are
for dev validation; the AWS deploy is how testers actually play. Standing warm capacity (`min ≥ 1`) is
an **opt-in config change** we make only when traffic warrants — **ask the user** before adding idle
cost. Make "first entrant warms it" *reliable*, since target-tracking won't wake an instance from
absolute zero: nudge `DesiredInstances` 0→1 from the backend on the first ticket (e.g. in the
matchmake Lambda) and set match/queue timeouts long enough to survive the boot; let target-tracking
handle scale-DOWN back to zero. (See `hoverball/service` §2.)

**The default recommendation for most teams:** start at "FlexMatch `WITH_QUEUE` + managed fleets +
serverless glue," **scale-from-zero**, and only move right on the specific rows where a real, named
requirement makes the managed default unacceptable. Both of our reference integrations are coherent
precisely because each sits where its game's needs actually are — not where the platform's defaults
happened to fall.

---

## Where to go next

- **Platform facts behind any of these choices** → `PLATFORM.md`.
- **Designing the queue/matchmaking spine itself** → `QUEUES.md`.
