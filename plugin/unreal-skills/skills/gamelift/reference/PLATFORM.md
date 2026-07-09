# Amazon GameLift — Platform Reference

> Status: Reference. Describes the platform as of June 2026, grounded in AWS documentation.
> This document is **about GameLift itself** — not about how any one project uses it. For the
> ways teams *assemble* GameLift into a hosting architecture, see `PATTERNS.md`; for the
> placement spine (queues + matchmaking) in depth, see `QUEUES.md`.

GameLift is AWS's managed service for **running session-based, real-time multiplayer game
servers**. It hosts your dedicated server process on compute it provisions, keeps a pool of
processes warm, finds a healthy host for each new match, and tracks the players in it. The
mental model to hold throughout: **GameLift is a placement-and-lifecycle layer over a pool of
server processes.** Everything it offers is some refinement of "pick a process, start a session
on it, tell players where to connect, recycle it when done."

The whole point of reading this before designing is that GameLift is **not one product with one
right usage** — it is a spectrum of managed conveniences, each of which you can adopt or replace
with your own code. The recurring design question is always *"do I let GameLift own this, or do I
own it?"* This document marks each capability **[MANAGED]** (AWS runs it) or **[YOURS]** (you must
build it), so that question is answerable feature by feature.

---

## 1. Naming and product boundaries (read first)

In March 2025 AWS rebranded the hosting service to **"Amazon GameLift Servers"** and split out a
**separate** product, **"Amazon GameLift Streams"** (pixel/application streaming — a different
thing entirely, with its own docs and pricing). "GameLift" is now an umbrella brand.

Three facts that prevent confusion:

- **Only the brand name changed for Servers.** The API, SDKs, CloudFormation namespace
  (`AWS::GameLift::*`), and service endpoint (`gamelift`) are unchanged. Code and IaC written
  before the rebrand still works.
- **Docs live at two hosts** — legacy `docs.aws.amazon.com/gamelift/...` and newer
  `docs.aws.amazon.com/gameliftservers/...`. Both are live and authoritative.
- **GameLift Streams is out of scope here.** When this document says "GameLift" it means GameLift
  Servers — dedicated game-server hosting. Never conflate the two.

A few stale claims circulate in older material; treat these as corrected:

| Stale claim | Reality |
|---|---|
| "Realtime Servers is deprecated." | False. Only its old Node.js 10 runtime (end of support Sep 30 2026) and Amazon Linux 2 (EOS Jun 30 2025) are retiring; the product remains. |
| "Use GameLift Local for SDK 5 testing." | GameLift Local is legacy (SDK 3/4 only). For SDK 5.x, an **Anywhere fleet** is the supported local-iteration path. |
| "Egress bandwidth is billed." | Data transfer is **free on gen-6+ instances**. |
| "`ActivateGameSession` takes arguments." | No-arg in SDK 5.x. |

---

## 2. The object model

The deployment chain is: **Build/Script → Fleet → Compute (× Locations) → GameSession →
PlayerSessions**, with an **Alias** as a stable pointer and a **GameSessionQueue** doing the
placing. Hold this chain in mind; almost every design decision is about which links you let
GameLift manage.

| Object | What it is | Notes |
|---|---|---|
| **Build** | Your packaged dedicated-server executable, uploaded and deployed to each instance. | Carries an OS (`WINDOWS_2016/2022`, `AMAZON_LINUX_2/2023`) and a server-SDK version. SDK ≥5 is current. |
| **Script** | The JavaScript bundle for a **Realtime Servers** fleet (the turnkey alternative to a custom build). | Used instead of Build for Realtime. |
| **Fleet** | A collection of compute running your server, optionally spread across multiple **Locations**. | One instance type per fleet. Three flavors: managed EC2, managed container, Anywhere (§3). |
| **Location** | A geographic place a fleet's resources live — an AWS Region, Local Zone, or custom (Anywhere) location. | A multi-location fleet has a home Region plus remote locations. Encoded in session ARNs. |
| **Compute** | The individual hosting resource. | EC2 fleet → an instance ID; container fleet → a container group; Anywhere → your registered compute name. |
| **Alias** | A stable pointer to a fleet (or routing strategy). | Build/fleet IDs change every deploy; the alias ID clients and queues reference **stays constant**. Point queues at aliases, not raw fleets. |
| **GameSession** | One running match on one server process. | Holds connection info (IP/DNS + port), player slots, and game data. A process hosts **one session at a time**. |
| **PlayerSession** | One reserved/active player slot within a game session. | The 1-game-session : N-player-sessions model (§5). |
| **GameSessionQueue** | The placement engine: given a request, it finds a healthy host across fleets/regions. | The heart of `QUEUES.md`. |
| **GameServerGroup / GameServer** | The **FleetIQ-standalone** object model — a separate world (§3.5). | Does **not** use queues or placement. Keep mentally distinct. |

**The single most important structural fact:** **one server process hosts exactly one game
session.** Concurrency on a host comes from running multiple processes (`ConcurrentExecutions`),
not from multiplexing sessions onto one process. Everything about capacity, scaling, and cost
derives from this.

---

## 3. Hosting flavors (where your server runs)

GameLift's current flavors are **Managed EC2, Managed containers, Anywhere, and Hybrid**, plus the
turnkey **Realtime** variant and the standalone **FleetIQ** model. Choosing among these is the
first and most consequential decision.

### 3.1 Managed EC2 fleets [MANAGED]
AWS-owned EC2 instances onto which GameLift deploys and manages your custom build. You pick
instance type, locations, and how many processes run per host. This is the default path for a
custom C++/C# (or Unreal) dedicated server.

- **On-Demand** — fixed cost, no interruption. Use where session continuity matters.
- **Spot** — 50–85% cheaper but reclaimable with a **2-minute notice**. GameLift adds a
  spot-viability algorithm that steers new sessions off at-risk instances. **Spot fleets must use
  a placement queue.**

### 3.2 Managed container fleets [MANAGED]
AWS-managed **Linux** instances running your **containerized** server (a Docker image in ECR,
orchestrated via ECS under the hood). Server containers are **replicated multiple times per
instance** for density; an optional per-instance "daemon" group runs utility software once per
host. Adds **rolling updates to live fleets** and decouples build deploy from fleet creation.
**Linux only — no Windows containers.** Choose this if you already live in containers/CI-CD and
can target Linux.

### 3.3 Anywhere fleets [YOURS compute, MANAGED control plane]
Register **your own hardware** — a workstation, on-prem servers, other clouds, your own EC2 — as a
logical fleet, while still using GameLift's session management, queues, FlexMatch, and metrics.
You own deployment, health, and scaling of that compute. Two dominant uses:

- **Local iteration / dev-test** — point an Anywhere fleet at your laptop and drive it through the
  real control plane. This is the **supported replacement for GameLift Local** on SDK 5.x.
- **Hybrid hosting** — mix your hardware with managed fleets for burst, gradual migration, or edge
  locations AWS Regions don't cover.

### 3.4 Realtime Servers [MANAGED, scripted]
A lightweight, ready-to-run server configured by a **JavaScript** script instead of a full custom
build. Good for session-based games exchanging small amounts of data (turn-based, card, light
real-time). Still offered. **Does not support FlexMatch backfill.** Choose it when you want a
turnkey server with a little custom logic and don't want to build/operate a C++/C# binary.

### 3.5 FleetIQ + Game Server Groups (standalone) [YOURS orchestration]
A fundamentally different shape. FleetIQ optimizes **low-cost EC2 Spot** while you **keep direct
ownership of your EC2 and EC2 Auto Scaling resources** (up to ~90% savings). Creating a
**GameServerGroup** provisions a linked Auto Scaling group **in your account** that you fully
control. You self-orchestrate placement and matching via `RegisterGameServer` / `ClaimGameServer`
— there are **no queues and no `StartGameSessionPlacement`** here. Choose this when you already run
your own EC2/ECS/EKS orchestration and want maximum control plus the cheapest Spot economics.

### 3.6 Quick chooser

| You want… | Flavor |
|---|---|
| Custom server, let AWS run the machines | Managed EC2 |
| Containerized server, rolling updates, Linux | Managed containers |
| Your own hardware / local dev / edge | Anywhere |
| Turnkey JS server, minimal ops | Realtime |
| Your own EC2 orchestration + cheapest Spot | FleetIQ GameServerGroups |
| Stream the whole game to a browser | GameLift **Streams** (different product) |

---

## 4. Server and backend SDKs

GameLift has **two distinct SDK surfaces**, and confusing them is a classic mistake.

**Server SDK [YOURS, in the game server].** Linked into your dedicated server (C++, C#, Unreal,
Go; Unity via C#). It is how a process tells GameLift "I'm alive and ready" and receives session
lifecycle callbacks. The core verbs:

- `InitSDK()` — initialize. **Two overloads, and picking the wrong one kills the process.** The
  no-arg overload is for managed fleets (reads parameters from the on-host GameLift agent); the
  `InitSDK(ServerParameters)` overload (WebSocket URL, auth token, compute/fleet IDs) is for
  Anywhere without the agent.
- `ProcessReady(...)` — declare the process ready to host and register its port and callbacks.
- Callbacks you implement: **onStartGameSession** (a session was placed here — read game data,
  then `ActivateGameSession()`), **onProcessTerminate** (drain and exit — also fires on Spot
  reclaim), **onHealthCheck** (return a bool within 60s; **3 consecutive failures → shutdown**),
  **onUpdateGameSession** (required for backfill).
- `AcceptPlayerSession()` / `RemovePlayerSession()` — validate connecting players against reserved
  slots. `ProcessEnding()` then `Destroy()` — graceful exit (Destroy is effectively required so
  normal exits aren't logged as crashes). `GetTerminationTime()` — the Spot deadline.

**Backend / service API [YOURS, in a trusted backend].** Part of the AWS SDK, called by **your
server-side code on the player's behalf** — never by the game client with AWS credentials. This is
how a match gets requested and a session gets created: `StartGameSessionPlacement` /
`DescribeGameSessionPlacement`, `CreateGameSession`, `StartMatchmaking`, `CreatePlayerSession(s)`,
`SearchGameSessions`. The game client only ever receives a plain IP/port + player-session ID and
connects directly.

**The Unreal plugin** bundles the C++ Server SDK v5 plus the backend AWS SDK C++ libraries, adds
editor workflows for Anywhere/managed-EC2/container hosting, and ships CloudFormation templates
(including a fuller scenario with a queue + FlexMatch). It is a convenience wrapper over the two
SDKs above, not a third thing.

---

## 5. The session and player-slot model

A **GameSession** holds **PlayerSessions** in a one-to-many relationship, and this is the unit of
truth GameLift tracks for occupancy.

- A PlayerSession moves **RESERVED → ACTIVE → COMPLETED**, or **TIMEDOUT** if the player never
  connects. The reservation window is **60 seconds**: the backend reserves a slot
  (`CreatePlayerSession`), the player connects, the server validates them
  (`AcceptPlayerSession`, RESERVED→ACTIVE); if they don't show, the slot reopens.
- A GameSession is **ACTIVATING → ACTIVE → TERMINATING → TERMINATED** (with an **ERROR** state
  too). It must be ACTIVE to hold players. Terminations carry a reason (`INTERRUPTED` for Spot,
  `TRIGGERED_ON_PROCESS_TERMINATE`, `FORCE_TERMINATED`).
- Slot accounting: `MaximumPlayerSessionCount` vs `CurrentPlayerSessionCount`, gated by
  `PlayerSessionCreationPolicy` (`ACCEPT_ALL | DENY_ALL`).
- **Game data into the session:** `GameProperties` (≤16 key-value pairs) and `GameSessionData`
  (one string, ≤256KB) are handed to the server at session start — the channel for passing
  per-match configuration (map, mode, your own identifiers) into the process.
- **Hard caps:** **200 player sessions per game session**; **50 server processes per instance**.

**Game session protection** (`FullProtection`) stops an ACTIVE session from being killed during
**scale-down** — but **does not protect against Spot reclaim**. This catches people: on Spot, even
a protected ACTIVE session can be terminated with reason `INTERRUPTED`.

**A reservation you build yourself is a separate thing from a GameLift PlayerSession.** Teams that
want their own overfill/last-slot-race guarantees often track reservations in their own backend
and use GameLift's player-session accounting loosely or not at all — see `PATTERNS.md`.

---

## 6. Scaling and capacity [MANAGED, if you let it]

- **Target-based auto-scaling** tracks `PercentAvailableGameSessions` (idle buffer) — you set a
  target percentage and GameLift keeps that much warm headroom. This is the recommended default.
- **Rule-based auto-scaling** — "if `<metric>` stays `<comparison> <threshold>` for `<period>`,
  change capacity by `<value>`." More control, more rope.
- **Manual capacity** (`UpdateFleetCapacity`) sets min/desired/max directly. **The defaults are
  min 0, max 1** — if you never raise max, auto-scaling does nothing. A team that wants to own
  scaling entirely drives DesiredInstances itself and leaves auto-scaling off.
- Capacity is tracked and scaled **per location**. A 10-minute cooldown follows each scaling
  action.
- **Cold start is real.** A new instance must launch, receive the build, and activate a process
  before it can host — seconds to minutes. This is *why* warm idle headroom exists; designs that
  assume instant capacity will strand players.

---

## 7. Observability [MANAGED]

GameLift publishes to **CloudWatch** under the `Amazon GameLift` namespace at 1-minute
granularity: fleet metrics (instances/processes/sessions/players), queue metrics (AverageWaitTime,
QueueDepth, placement successes/failures, FirstChoiceNotViable), and matchmaking metrics
(TicketsStarted, TimeToMatch, MatchesCreated/Accepted). Placement and matchmaking **events** are
emitted to **EventBridge** automatically and optionally to **SNS** — the basis for event-driven
backends (§ `QUEUES.md`). Server logs upload to GameLift and are retained 14 days.

---

## 8. Deployment and infrastructure-as-code

You can stand up GameLift through the **CLI** (`create-build`/`upload-build`, `create-fleet`,
`register-compute`), **CloudFormation** (the full `AWS::GameLift::*` resource set), **CDK** (L2
constructs are in `aws-gamelift-alpha` — still alpha; L1 `Cfn*` are stable), **Terraform**
(`hashicorp/aws` for core types, `awscc`/Cloud Control for newer ones), and **Pulumi** (classic
`@pulumi/aws`, which wraps `hashicorp/aws`, or Cloud-Control `aws-native`). The Unreal/Unity plugins
also ship CloudFormation templates. **Provider caveat:** the classic Terraform/Pulumi GameLift
resources lag the API — e.g. `Build` lacks `server_sdk_version` (see below), and
`MatchmakingRuleSet`/`MatchmakingConfiguration` are deprecated in `@pulumi/aws` (slated to move to
`aws-native`). For fields the classic provider is missing, drop to the CLI or a Cloud-Control
resource for that one piece.

Two deployment facts worth designing around:

- **Build upload is its own step.** `aws gamelift upload-build` uploads the files and registers the
  Build in one move; the lower-level path is `CreateBuild` + a manual S3 upload. Either way, the
  **build must exist before the fleet that deploys it** — this ordering often forces a two-phase
  IaC apply (provision the bucket/roles first, upload, then create Build → Fleet → Alias).
- **The classic Terraform/Pulumi `Build` resource can't set the server-SDK version.**
  `hashicorp/aws`'s `aws_gamelift_build` (and Pulumi's classic `aws.gamelift.Build`, which wraps it)
  exposes only an OS + a free-text `version` *label* — there is **no `server_sdk_version` field**, so
  a Build created that way is tagged with the default (SDK 4.x). **`AMAZON_LINUX_2023` fleets reject
  an SDK-4 build**, so this silently produces a fleet that never activates. CloudFormation
  (`AWS::GameLift::Build` has `ServerSdkVersion`), CDK, and the Cloud-Control providers (`awscc` /
  Pulumi `aws-native`) *do* expose it. **Real-world fix** (used by `hoverball/service`, see `PATTERNS.md`):
  create the build out-of-band with `aws gamelift upload-build --server-sdk-version 5.x
  --operating-system AMAZON_LINUX_2023` and feed the returned build ID to the fleet, instead of
  letting the classic IaC resource create the Build. (Windows / AL2 builds are SDK-version-agnostic
  enough that the classic resource is fine.)
- **`RuntimeConfiguration` defines the processes per host** — one line per executable with a
  `LaunchPath`, `Parameters`, and `ConcurrentExecutions`. Concurrent sessions per instance = the
  sum of `ConcurrentExecutions`. The hard cap is 50 processes/instance.

For local iteration on SDK 5.x, use an **Anywhere fleet** (the supported path), not the legacy
GameLift Local jar.

---

## 9. Pricing — the shape, not the numbers

Verify exact figures on the live calculator; the *shape* is what informs architecture:

- **Primary cost = instance hours**, varying by Region, instance type/size, OS (Windows ~2× Linux
  for licensing), and On-Demand vs Spot.
- **Spot** is 50–85% cheaper, billed at the Spot price at each hour's start, never above
  On-Demand.
- **Data transfer is free on gen-6+ instances** (legacy egress charges are gone).
- **FlexMatch is free with managed hosting**; **standalone FlexMatch** bills on Player Packages +
  Matchmaking Hours.
- **Queues have no separate line item** — you pay for the underlying fleet instances.
- **FleetIQ** is a management fee layered over EC2 you own.

The architectural takeaway: **idle instances cost money; scale-from-zero saves it but adds
cold-start latency.** Most cost decisions are really this tradeoff plus the Spot-vs-On-Demand one.

---

## 10. Limits and gotchas worth designing around

| Gotcha | Why it bites |
|---|---|
| **One process = one session.** | Concurrency = more processes, not session multiplexing. Drives all capacity math. |
| **Session-oriented model.** | GameLift expects sessions to *end*. Persistent worlds/MMOs fight this with long-lived sessions or sharding (see `PATTERNS.md`). |
| **Spot reclaim ≠ protected.** | `FullProtection` covers scale-down, not Spot interruption. Don't run long, non-resumable, latency-critical sessions on Spot. |
| **Cold start.** | New capacity takes seconds-to-minutes. Keep warm headroom or hold the player's request across the boot. |
| **Caps.** | 200 player sessions/session, 50 processes/instance, 100GB build storage/Region — all hard. Fleet/queue/config counts are adjustable quotas. |
| **New-account fleet limits.** | Real-world accounts have hit a hard cap of **1 fleet** despite a higher Service-Quotas value, resolved only via an AWS Support case. Budget time for a quota fight before assuming N fleets. |
| **Region availability is dimension-dependent.** | ~14 home Regions for fleets, but many more deployable *locations* (remote locations, Local Zones). Be explicit which you mean. |
| **Two SDK surfaces.** | Server SDK (in the server) vs backend service API (in your backend, never the client). Mixing them up is a security and correctness bug. |
| **`InitSDK` overload mismatch.** | Calling the Anywhere overload on a managed fleet (or vice versa) makes the host agent terminate the process within ~1 second. |
| **Classic IaC `Build` can't tag the server-SDK version.** | `hashicorp/aws` / Pulumi-classic `aws.gamelift.Build` has no `server_sdk_version` → defaults to SDK 4, which **AL2023 fleets reject** (fleet never activates). Use `upload-build --server-sdk-version` or CloudFormation/aws-native. See §8. |
| **Uncooked content won't run a dedicated server.** | A Development server run from raw `Binaries/` against uncooked editor content crashes in async package loading. Run a **cooked/staged** build (`BuildCookRun -server -cook -stage`) — which is the deploy artifact anyway. |

---

## Where to go next

- **Designing the placement/matchmaking spine** ("how do I set up queues?") → `QUEUES.md`.
- **Choosing how much to own vs delegate, with two real reference integrations** → `PATTERNS.md`.
