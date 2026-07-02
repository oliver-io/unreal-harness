# The Placement Spine — Game Session Queues and FlexMatch

> Status: Reference. The "how do I set up queues?" document. Code-free and conceptual.
> Background object model is in `PLATFORM.md`; the full ownership tradeoff is in `PATTERNS.md`.

"Setting up a queue" in GameLift almost always means assembling some part of a three-stage
**placement spine**:

```
   players ──▶ [ MATCHMAKER ] ──▶ [ QUEUE ] ──▶ [ FLEET / COMPUTE ]
              forms a match      finds a host    runs the session
              (who plays         (where it        (the actual
               together)          runs)            game server)
```

The central insight for a queue designer: **these three stages are separable, and you can let
GameLift own any subset of them.** A queue (stage 2) can exist with or without a matchmaker in
front of it. A matchmaker can place into a queue or hand results back to you. The art of "setting
up queues" is deciding which boxes are GameLift's and which are yours. This document describes each
box, then the legal ways to wire them together.

---

## 1. The GameSessionQueue (stage 2 — placement)

A **GameSessionQueue** answers one question: *given a request for a new session, which host should
run it?* It is the single most useful managed component for most teams, because it absorbs
multi-fleet, multi-region, cost, and latency complexity that is painful to build yourself.

### What a queue is made of

| Element | Role |
|---|---|
| **Destinations** | An ordered list of fleets and/or aliases (by ARN) the queue may place into. **Best practice: list aliases, not raw fleets**, so build/fleet swaps don't rewrite the queue. |
| **Priority configuration** | The ordered criteria the queue uses to choose among viable locations (below). |
| **Location order / filter** | Where to look first, and an allow-list that can exclude a troubled location without redeploying. |
| **Player latency policies** | Hard caps on acceptable latency; a placement that can't meet them times out. Multiple policies form a cap that **relaxes over time** the longer a player waits. |
| **Timeout** | How long the queue tries before the placement `TIMED_OUT`. |
| **Notification target** | An optional SNS topic for placement events (EventBridge gets them regardless). |

### How a queue chooses (priority configuration)

The default ordering, reorderable, with any omitted criterion appended in default order:

1. **Latency** — if the request carries per-player latency data, prefer the lowest-average-latency
   location.
2. **Cost** — otherwise (or to break ties), prefer cheaper hosting (Spot vs On-Demand, instance
   type, location).
3. **Destination** — break remaining ties by the destination order you listed.
4. **Location** — final tie-break, alphabetically by location.

The practical consequences:

- **A single queue can span regions.** Its destinations can each be multi-location fleets; the
  queue evaluates all locations across all destinations and falls through preferences when the top
  choice can't place. This is how one queue gives global coverage with regional failover.
- **Latency-based region selection is driven by the request, not the queue.** Your backend supplies
  per-player latency measurements *per candidate location* (AWS recommends UDP ping beacons);
  GameLift averages them and, when Latency is the top priority, picks the best location subject to
  the latency policies.
- **A degenerate queue is legal and common.** A queue with **one destination and no latency rules**
  does no real selection — it is a one-hop pipe from "place this" to "run it here." Teams that own
  their own placement logic deliberately use queues this way (see `PATTERNS.md`, the control-plane
  pattern). Nothing is wrong with this; it's just a different ownership choice.

### Driving the queue (the placement lifecycle)

Your backend starts a placement with **`StartGameSessionPlacement`** (queue name, a unique
PlacementId, max players, and optional game data / desired player sessions / per-player latencies).
The placement then moves through:

**PENDING → FULFILLED**, or **CANCELLED / TIMED_OUT / FAILED**.

- While **PENDING**, the connection details are **not final** — GameLift may try several hosts.
- On **FULFILLED**, the session exists and the IP/DNS, port, region, and reserved player sessions
  are final. **Only now may a client connect.**
- **TIMED_OUT** placements are resubmittable — a common resilience pattern is to resubmit with a
  fresh PlacementId rather than fail the player, especially when capacity is scaling from zero.

**Do not poll if you can help it.** GameLift emits placement events to **EventBridge** (and
optionally **SNS**): `PlacementFulfilled` (carries the full connection info — port, session ARN,
IP/DNS, location, placed player sessions), `PlacementCancelled`, `PlacementTimedOut`,
`PlacementFailed`. The idiomatic backend is **EventBridge rule → Lambda → write connection info to
a store the client polls** — not a tight `DescribeGameSessionPlacement` loop.

---

## 2. FlexMatch (stage 1 — matchmaking)

**FlexMatch** answers a different question: *which players should be in the same match, and on which
teams?* It is optional. A game that just needs "drop the next request onto any free host" needs
only a queue. A game that needs "pair these two," "balance these teams by skill," or "respect
parties and latency" wants FlexMatch.

### What you configure

- **A MatchmakingRuleSet** — the JSON that defines teams (`name`, `minPlayers`, `maxPlayers`,
  `quantity`), player attributes, and rules. Rule types range from a trivial **count** ("two
  players, one team") up to skill/latency/collection/compound logic, with **expansions** that relax
  criteria as a partial match ages ("after 30s, widen the skill band"). Small matches go up to **40
  players**; large matches up to **200** with post-match team balancing.
- **A MatchmakingConfiguration** — the "matchmaker" that binds a rule set to an operating mode and
  the surrounding knobs: `AcceptanceRequired` (+ timeout) for a ready-up handshake,
  `RequestTimeoutSeconds` (ticket lifetime, up to 12h), `BackfillMode` (`AUTOMATIC | MANUAL`),
  `AdditionalPlayerCount` (slots held open for backfill), and a notification target.

### The ticket lifecycle

A matchmaking request is a **MatchmakingTicket**, driven from your backend (never the client):

**QUEUED → SEARCHING → [REQUIRES_ACCEPTANCE] → PLACING → COMPLETED**, or **FAILED / CANCELLED /
TIMED_OUT**.

- `REQUIRES_ACCEPTANCE` appears only if you required acceptance; every player calls `AcceptMatch`,
  and any reject/timeout drops the match (accepted players' tickets return to SEARCHING).
- On **COMPLETED**, the ticket carries the connection info and each player's player-session ID.
- The match is described to the **server** via `MatchmakerData` (teams → players → attributes),
  so the server knows who is on which side.

FlexMatch also emits events to EventBridge/SNS (`MatchmakingSearching`, `PotentialMatchCreated`,
`AcceptMatch`, `MatchmakingSucceeded`, `MatchmakingTimedOut`, …) — again, prefer events over
polling.

### The pivotal knob: WITH_QUEUE vs STANDALONE

This single setting decides whether stages 1 and 2 are wired together by GameLift or by you.

| Mode | What GameLift does | What you do |
|---|---|---|
| **WITH_QUEUE** [MANAGED] | On a match, automatically hands results to a configured GameSessionQueue, which places the session; then updates **every** matched ticket with connection info + player session IDs. | Almost nothing — start tickets, read results. |
| **STANDALONE** [YOURS] | Forms the match and returns players + team assignments + a suggested location in a `MatchmakingSucceeded` event. **No placement.** | Build the placement service: find a host, create the session, hand back connection info. |

`WITH_QUEUE` is the "let GameLift run the whole spine" choice. `STANDALONE` is "use GameLift only as
a matcher and own placement yourself." Most teams that adopt FlexMatch use `WITH_QUEUE`.

### Backfill

Filling an existing FlexMatch-created session with late joiners, using the same matchmaker. **Only
works on FlexMatch-created sessions; not available for Realtime.** `AUTOMATIC` (hosted mode) backfills
open slots using the reserved `AdditionalPlayerCount`; `MANUAL` means your own logic (backend or
server) calls `StartMatchBackfill` on a dropout. Turn backfill **off** for fixed, closed matches
(e.g. a 1v1) — it's machinery you won't use.

---

## 3. Wiring the spine — the legal topologies

These are the standard ways to assemble the three stages. Each is a valid "queue setup"; they
differ only in where the GameLift/you boundary falls. `PATTERNS.md` maps real projects onto these.

| Topology | Stage 1 (match) | Stage 2 (place) | When to use |
|---|---|---|---|
| **Queue only** | none | GameSessionQueue you drive with `StartGameSessionPlacement` | You decide *who* plays together yourself (or it doesn't matter — e.g. join-any-server), but want GameLift to handle multi-region/cost/latency host selection. |
| **FlexMatch WITH_QUEUE** | FlexMatch | the queue, automatically | The "let GameLift do it all" path. You want matchmaking *and* placement managed. Smallest backend. |
| **FlexMatch STANDALONE** | FlexMatch | **yours** | You want GameLift's matchmaking rules but must place sessions with custom logic (special host selection, your own fleet accounting, non-GameLift compute). |
| **Custom matcher + queue** | **yours** | GameSessionQueue | You have bespoke matchmaking (cohorts, parties, your own skill system) but still want GameLift's placement intelligence. |
| **Custom matcher + direct create** | **yours** | **yours** (`CreateGameSession` straight to a fleet, or FleetIQ `ClaimGameServer`) | You own the whole spine; GameLift is just managed compute. Maximum control, maximum code. |

**A queue is not required at all.** `CreateGameSession` can target a fleet or alias directly,
skipping queues entirely — appropriate when you've already chosen the exact host. And the **FleetIQ
GameServerGroup** model (`PLATFORM.md` §3.5) replaces this entire spine with
`RegisterGameServer`/`ClaimGameServer` and no queue.

---

## 4. A queue-design checklist

When advising someone "setting up a queue," walk these decisions in order:

1. **Do you need matchmaking, or just placement?** If players don't need to be *grouped* (they join
   any available server, or you group them yourself), you may need only a queue — skip FlexMatch.
2. **If matchmaking: WITH_QUEUE or STANDALONE?** Default to WITH_QUEUE unless you have a concrete
   reason to own placement.
3. **One region or many?** Multi-region = multi-location fleets behind one queue + per-player
   latency data in the request + latency as the top priority criterion.
4. **What's the destination list?** Aliases, not raw fleets. One destination = a simple pipe; many
   = real selection.
5. **On-Demand, Spot, or both?** Spot needs a queue (mandatory) and graceful `onProcessTerminate`
   handling. Mixed destinations let the cost-priority criterion prefer Spot with On-Demand
   failover.
6. **Latency policy?** Set caps only if you'll enforce them; remember they relax over wait time.
7. **Timeout + cold-start behavior.** If fleets scale from zero, the first placement can outlive a
   naive timeout — plan to resubmit `TIMED_OUT` placements (with a fresh PlacementId) and hold the
   player's request across the boot rather than failing them.
8. **Events, not polling.** Wire `PlacementFulfilled` / `MatchmakingSucceeded` through
   EventBridge/SNS to whatever the client reads.
9. **Backfill: on or off?** Off for fixed closed matches; AUTOMATIC/MANUAL only if late joiners
   make sense.
10. **Who tracks occupancy?** GameLift's PlayerSession accounting, your own reservation system, or
    both — and is there a last-slot race two requests could both win? (See `PATTERNS.md`.)

---

## 5. Common failure modes

| Symptom | Usual cause |
|---|---|
| Placements `TIMED_OUT` forever on a fresh deploy | Fleet `MaximumPlayerSessionCount`/capacity is min 0 / max 1 and never raised; or scale-from-zero cold start exceeds the queue timeout with no resubmit. |
| Every match `TIMED_OUT` on one shared queue | A long-lived/persistent session permanently holds the only process when `ConcurrentExecutions` is 1; split roles into separate fleets/queues. |
| Client connects to stale/dead host | Acted on connection info before the placement was `FULFILLED`, or routed from a cache that outlived the session. |
| Players placed in the wrong region | No per-player latency data supplied, or Latency isn't the top priority criterion. |
| Spot sessions die unexpectedly mid-match | Expected — handle `onProcessTerminate` + `GetTerminationTime`; don't rely on session protection (it doesn't cover Spot). |
| Backfill never fires | Session wasn't FlexMatch-created, or it's a Realtime fleet (unsupported), or `AdditionalPlayerCount` is 0. |

---

## Where to go next

- **The full object model and platform facts** → `PLATFORM.md`.
- **Two real integrations mapped onto the topologies above, plus the ownership tradeoff** →
  `PATTERNS.md`.
