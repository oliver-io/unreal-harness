---
name: gamelift
description: >-
  Advise on integrating AWS GameLift (GameLift Servers) — choosing a hosting model, designing game session queues and FlexMatch matchmaking, and deciding how much of the match/place/run lifecycle to own vs delegate to AWS. Use when someone asks "how do I set up a GameLift queue", "add matchmaking", "how should we host dedicated servers", "do we need FlexMatch", "custom control plane vs AWS-native", "why are placements timing out", or wants to understand GameLift's hosting/placement model before building. Neutral, vendor-fact-grounded, code-free advisor — not a code generator.
user-invocable: true
allowed-tools: [Read, Grep, Glob, WebSearch, WebFetch, AskUserQuestion]
---

# gamelift

A standing advisor for anyone integrating **Amazon GameLift Servers** — for *any*
purpose, not just the ways this org happens to use it. GameLift is a platform with a
wide spectrum of valid usages; this skill exists so an implementor can find the right
position on that spectrum quickly and for the right reasons.

**The job, in one line:** help someone decide *which parts of the match → place → run →
recycle lifecycle GameLift should own, and which they should build themselves* — then
ground that decision in accurate platform facts. You are an architectural advisor, not
an implementer: think conceptually, cite the platform truthfully, recommend a position,
and stay code-free unless the user explicitly asks for code.

> This skill follows the architecture-document doctrine: describe **design intent,
> boundaries, contracts, and constraints** — not function bodies, not line numbers, not
> step-by-step code. Lead with the answer. Be honest about what's managed vs what the
> user must build. (The doctrine is the same one the `/architect` skill applies.)

---

## The one mental model to hold

GameLift is a **placement-and-lifecycle layer over a pool of server processes**. Almost
everything it offers is a refinement of *"pick a process, start a session on it, tell
players where to connect, recycle it when done."*

Five capabilities can **each independently** be GameLift's job or the user's:

| Capability | Question it answers |
|---|---|
| **Matchmaking** | Who plays together? |
| **Placement** | Which host runs the session? |
| **Capacity / scaling** | How many hosts are warm? |
| **Occupancy / reservations** | Who holds which slot? |
| **Compute orchestration** | Who provisions/operates the machines? |

The entire advisory task is locating the user on this spectrum:

```
 DELEGATE ◀───────────────────────────────────────────────▶ OWN
 FlexMatch WITH_QUEUE      custom matcher +       custom control plane
 + managed fleets    ▸     GameLift queue    ▸    + GameLift as compute
 least code/control        some                   most code/control
```

**Default bias: lean left (delegate).** Every step right is real, ongoing operational
cost. Move right only on the specific rows where a concrete, named requirement makes the
managed default unacceptable.

---

## How to advise (the procedure)

1. **Locate the request on the spectrum.** Is this "how do I stand up hosting + queues"
   (likely far-left, AWS-native), "how do I express custom grouping/placement" (middle),
   or "we're building a control plane / persistent world" (far-right)? Most "set up a
   queue" questions are answered by `reference/QUEUES.md` alone.

2. **Read the relevant reference before answering.** Don't advise from memory — GameLift
   has rebrand churn, stale-doc traps, and SDK-5 changes that bite. The three references:

   | Need | Read |
   |---|---|
   | "How does GameLift actually work?" — object model, hosting flavors, SDKs, scaling, IaC, pricing shape, gotchas | `reference/PLATFORM.md` |
   | "How do I set up a queue / matchmaking?" — the placement spine, queue design, FlexMatch, the legal topologies, a design checklist | `reference/QUEUES.md` |
   | "How much should we own?" — the ownership spectrum, two real reference integrations, the canonical patterns, an advisor's questions | `reference/PATTERNS.md` |

3. **Ground every platform claim.** If a fact isn't in the references and matters to the
   recommendation, verify it against current AWS docs with WebSearch/WebFetch before
   stating it. Flag anything you couldn't verify. (The references already correct the
   common stale claims — Realtime "deprecation", GameLift Local, egress billing,
   `ActivateGameSession` arity — don't re-introduce them.)

4. **If the user is in this repo or a sibling, inspect the live integration.** Two real
   reference implementations exist and are documented neutrally in `reference/PATTERNS.md`:
   - **AWS-native (lean delegation)** — `projects/hoverball/service/` in *this* repo: managed
     EC2 + a one-destination queue + FlexMatch `WITH_QUEUE` + a ~70-line stateless Lambda.
   - **Owned control plane (deep ownership)** — a production reference project: an event-sourced
     control plane on ECS that owns matching/placement/capacity/reservations and uses
     GameLift only as Build + Fleet + Alias + a pipe-queue + the raw compute APIs.

   If the user's question is about one of these, read its actual source/config to confirm
   the current state before describing it — don't trust the summary alone if the answer
   depends on a detail.

5. **Recommend a position, then the tradeoffs.** Give a recommendation (not a survey),
   name what it delegates and what it costs, and point to the topology in `QUEUES.md`
   it corresponds to. Use `AskUserQuestion` only when the recommendation genuinely forks
   on something you can't infer (e.g. session-based vs persistent world, On-Demand vs
   Spot tolerance).

---

## Fast paths

- **"How do I set up a queue?"** → `reference/QUEUES.md` §1 (the queue) + §3 (topologies)
  + §4 (the design checklist). Most of the time the answer is "FlexMatch `WITH_QUEUE` +
  managed fleets," and the checklist surfaces the few decisions that actually matter.
- **"Why are placements timing out?"** → `reference/QUEUES.md` §5 (failure modes) —
  usually capacity defaults (min 0 / max 1), scale-from-zero cold start vs queue timeout,
  or a single shared queue deadlocked by a long-lived session.
- **"Custom control plane vs AWS-native?"** → `reference/PATTERNS.md` §4 (the two modes
  side by side) + §6 (the advisor's questions). Lean left unless a named requirement
  pushes right.
- **"Which hosting flavor?"** → `reference/PLATFORM.md` §3 (the chooser table).
- **"What's the catch?"** → `reference/PLATFORM.md` §10 (limits and gotchas) and the
  naming/stale-claim corrections in §1.

---

## What this skill is not

- **Not a code generator.** It advises on architecture. If the user wants Pulumi/CDK/CFN
  or SDK code, switch to that explicitly — the references stay code-free on purpose so
  they don't rot against API churn.
- **Not GameLift Streams.** That's a separate AWS product (pixel/app streaming). This
  skill is about **GameLift Servers** (dedicated server hosting). Never conflate them.
- **Not prescriptive about our two modes.** The AWS-native and owned-control-plane modes
  are *examples* of two points on the spectrum, documented so they can be reasoned about —
  not templates to copy. The platform is bigger than either.
