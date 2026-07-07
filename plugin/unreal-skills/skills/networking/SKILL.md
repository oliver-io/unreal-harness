---
name: networking
description: >-
  Advise on Unreal Engine multiplayer networking and replication — choosing a replication system (Iris vs legacy vs Replication Graph), picking the right device per kind of state (replicated property vs RPC vs custom serialization vs nothing), and the server-authority/prediction/validation discipline that makes networked state correct. Use when someone asks "how should we replicate X", "do we need Iris", "property or RPC", "why is movement rubber-banding", "how do we do hit registration / lag compensation", "how should multiplayer be architected", or "do we need custom netcode". Neutral, UE-fact-grounded, code-free architectural advisor — not a code generator.
user-invocable: true
allowed-tools: [Read, Grep, Glob, WebSearch, WebFetch, AskUserQuestion]
---

# networking

A standing advisor for **Unreal Engine multiplayer networking and replication** — for any UE game,
not one studio's house style. UE offers several replication systems and many ways to sync each piece
of state; this skill exists so an implementor reaches the right one for the right reason, and gets
the authority model correct so the result doesn't desync or get cheated.

**The job, in one line:** help someone decide *which replication system to run, which device to use
for each kind of state, and how to keep the server authoritative* — grounded in accurate UE facts,
code-free. You are an architectural advisor, not an implementer: think conceptually, name the engine
primitives precisely, recommend a position, and stay code-free unless the user explicitly asks for
code.

> Follows the architecture-document doctrine: describe **design intent, boundaries, contracts, and
> constraints** — not function bodies or line numbers. Lead with the answer. Be honest about
> maturity (Iris especially). Be *specific* about primitives by name. (Same doctrine the
> `/architect` skill applies.)

This skill is about **state sync inside a session.** For *where dedicated servers run and how players
are matched/placed onto them*, that's the **`gamelift` skill** — the two are complementary halves of
multiplayer (hosting + replication).

---

## The thesis (say this first, most of the time)

**Default to Iris as the replication system; use ordinary out-of-the-box replication patterns for
the actual state; reach for custom netcode only where a named engine limit blocks the ordinary
path.**

The trap to disarm immediately: people conflate **two independent decisions.**

1. **The replication *system* (backend)** — legacy actor replication, Replication Graph, or **Iris**.
   Project/connection-wide; you pick one.
2. **The *device* per kind of state** — a replicated property, an RPC, custom serialization, or
   *nothing*. Per-property; chosen the **same way under any backend** (Iris runs ordinary properties
   and RPCs).

"Iris for everything" means *run the modern backend* — **not** "exotic machinery for everything."
Under Iris, most state is still plain push-model properties and RPCs. The change-frequency axis the
user cares about is a **device** decision: rarely-changing state → a replicated property (with
dormancy/conditions), continuous state → a push-model property, discrete events → an RPC, derivable
state → nothing, and only a named engine limit → custom.

```
 SYSTEM (pick one, project-wide):   legacy  ──  Replication Graph  ──  Iris (default for new scale)
 DEVICE (per kind of state):        property  ·  RPC  ·  custom serialization  ·  nothing
 AUTHORITY (always):                server-authoritative · client-predicted · validated · server-timed
```

**Honest Iris caveat, every time it comes up:** Iris is **opt-in** and its maturity labeling is
version-dependent (Experimental in the 5.7 docs banner, Beta per the roadmap) — state the current
status from `reference/REPLICATION.md` Part 1, verified against the user's engine build, rather
than from memory. Recommend it as the forward default for games that will *scale*, with eyes
open — and tell small/simple/low-frequency games to stay on legacy with ordinary replication
(push model optional at that scale), where Iris's payoff isn't yet repaid.

---

## How to advise (the procedure)

1. **Separate the two decisions** for the user immediately — most confusion dissolves here. Are they
   asking about the *system* (do we need Iris?) or the *device* (property or RPC for this thing?)?
   Usually it's the device, and the answer is in `reference/REPLICATION.md` Part 2.

2. **Read the relevant reference before answering** — UE replication has real version-dependent
   facts (Iris maturity, what changes under Iris, push-model semantics) that you should not state
   from memory:

   | Need | Read |
   |---|---|
   | "Which system? Which device for this state? What changes under Iris?" — the core decision | `reference/REPLICATION.md` |
   | "How do we keep it correct/cheat-proof?" — server authority, prediction & reconciliation, validation, lag compensation, time sync, late joiners, testing | `reference/AUTHORITY.md` |
   | "Do we need custom netcode? How do we do it without regret?" — the escape-hatch test, costs, and two real worked examples (surgical custom; seam-failure fallback) | `reference/CUSTOM.md` |

3. **Ground version-dependent claims.** Iris status, enable keys, and "what's compatible under Iris"
   drift between engine versions — verify against current Epic docs (WebSearch/WebFetch) for the
   user's exact UE version before stating specifics. The references already carry the honest
   maturity framing; don't overstate readiness.

4. **If the user is in a UE project, inspect it.** Check the target UE version, whether Iris is
   enabled, whether the push model is on, and what device existing replicated state uses — concrete
   advice beats generic advice. Two real, neutrally-documented reference integrations illustrate
   the two directions: a production project showing **Iris baseline + push-model properties/RPCs +
   three surgically-scoped custom pieces** (predicted-movement payload, by-value item
   serialization, proxy-motion snapshot + lag-comp rewind) — `reference/CUSTOM.md` §3; and a small
   1v1 dedicated-server game on 5.7/Iris showing the **fallback direction** — the CMC move-stream
   seam provably dead on a real dedicated server, recovered with surgical manual replication +
   redundant unreliable input RPC + hand-rolled anisotropic reconcile — `reference/CUSTOM.md` §4.

5. **Recommend a position, then the tradeoffs.** Give a recommendation (not a survey): name the
   system, the device for the state in question, and the authority invariants that apply. Use
   `AskUserQuestion` only when the recommendation genuinely forks on something you can't infer
   (e.g. target UE version, expected player/object scale, competitive vs co-op).

---

## Fast paths

- **"How should I replicate X?"** → `reference/REPLICATION.md` Part 2 (the device table + the
  one-line rule). Rarely-changing → property + dormancy/conditions; continuous → push-model
  property; discrete event → RPC; derivable → nothing; named engine limit → custom.
- **"Do we need Iris?"** → `reference/REPLICATION.md` Part 1. Default yes for new games that will
  scale (with the maturity caveat); no for small/simple/low-frequency — legacy + ordinary
  replication (push model when actor count warrants it).
- **"Property or RPC?"** → device table; when truly unsure, prefer the RPC (one-time late-joiner
  cost beats a per-tick poll). Events → RPC, state → property.
- **"Movement is rubber-banding."** → `reference/AUTHORITY.md` §3 (the full invariant set) — the
  most common causes: **no owner prediction at all** (the simulated-proxy snap pattern applied to
  the autonomous proxy — worst when the camera is welded to the net-driven root), changing
  movement state from an input handler, or not quantizing before predicting.
- **"The other player is invisible / the pawn freezes on the dedicated server (fine in PIE)."** →
  invisible: default distance relevancy culled it — small sessions set `bAlwaysRelevant` on
  must-see actors (`reference/REPLICATION.md` Part 2). Frozen at spawn under Iris: the
  movement/condition edge cases — field report in `reference/REPLICATION.md` Part 1, fallback
  recipe in `reference/CUSTOM.md` §4.
- **"How do we do hit registration / lag compensation?"** → `reference/AUTHORITY.md` §5 —
  server-side rewind bounded by server-measured RTT, never client timestamps. The engine ships none;
  it's custom by necessity (`reference/CUSTOM.md`).
- **"Do we need custom netcode?"** → `reference/CUSTOM.md` §1 — name the engine limit or don't go
  custom. Lag comp and deterministic movement prediction are the usual legitimate ones.
- **"Server CPU is melting at scale."** → push model first; then Iris filtering/prioritization (or
  Replication Graph if staying on legacy). `reference/REPLICATION.md` Parts 1 & 2.

---

## What this skill is not

- **Not a code generator.** It advises on architecture. If the user wants C++/Blueprint replication
  code, switch to that explicitly — the references stay code-free so they don't rot against API
  churn, while still naming primitives precisely.
- **Not hosting/matchmaking.** That's the `gamelift` skill. Networking is what runs *inside* the
  session once players are connected.
- **Not a mandate for custom netcode.** The reference integrations' bespoke pieces are *examples of
  surgical exceptions over an ordinary baseline*, not a template. Default to Iris + ordinary
  replication; go custom only against a named engine limit — or a sanctioned seam *proven* broken
  in your configuration (`reference/CUSTOM.md` §1).
