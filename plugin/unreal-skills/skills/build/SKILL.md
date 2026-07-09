---
name: build
description: Build an Unreal Engine project across its flavors — the editor target for local play-in-editor (PIE), a standalone/packaged Windows client, and a dedicated server (local dev or a cooked Linux build for remote/cloud hosting). Use when someone wants to "build the game", "build the editor", "compile the project", "package for Windows", "cook content", "make a client/server build", "build the dedicated server", "make a shipping build", "build for the cloud/GameLift", or asks which build command they need and what config/platform to use. Generic across any UE project with similar Editor/Game/Server target architecture — it derives names from the project, hardcodes none. Knows the compile-vs-cook-vs-package distinction, the target matrix, and the constraints that silently break builds.
user-invocable: true
allowed-tools: [Read, Grep, Glob, Bash, AskUserQuestion]
---

# build

How to build an Unreal Engine project — correctly, for the *flavor* the user actually
needs. "Build" is not one operation; it's a small matrix of (target × configuration ×
platform × how-far-down-the-pipeline). The whole job of this skill is to locate the
user in that matrix, run the right command, and respect the handful of constraints
that silently produce a broken or wrong build.

> This skill follows the same architecture-document doctrine as the `/architect`
> skill: describe **design intent, contracts, and
> constraints** — the *concepts and the stable command shapes*, not a brittle
> step-by-step for one project. Command-line invocation patterns (UBT/UAT syntax,
> BuildCookRun flag sets) are *contracts* — stable across refactors — so they belong
> here, the way struct fields do. Anything project-specific (the exact target name, the
> map list, the fleet wiring) is derived at run time, never baked in.

This skill is generic across any UE project whose source declares the usual targets
(an Editor target, a Game/Client target, optionally a Server target). It reads the
project to learn its real names; it assumes nothing about a particular game.

---

## The one mental model to hold

An Unreal build is a **pipeline of three independent stages**. Knowing which stage(s)
a request needs is 80% of picking the right command.

| Stage | What it produces | Tool | When you need it |
|---|---|---|---|
| **Compile** | A native binary (`.exe`, `.so`, editor module DLLs) from C++ | **UBT** (`Build.bat`) | Always — code changed, or no binary exists yet |
| **Cook** | Platform-baked content (assets serialized for a target platform) | the editor/UAT cooker | Running outside the editor (standalone/server), or packaging |
| **Stage / package** | A self-contained, distributable layout (paks, IoStore, prereqs) | **UAT** (`RunUAT BuildCookRun`) | Shipping a client or a deployable server |

The decisive question is **how far down the pipeline** the user needs to go:

```
 LEAST                                                                  MOST
 compile only         compile + loose cook        full UAT BuildCookRun
 (PIE, iteration)  ▸  (run standalone locally)  ▸  (-build -cook -stage -pak …)
 fast, in-editor      fast, no paks, loose files   slow, distributable, paks
```

**Default bias: go only as far as the goal requires.** PIE needs nothing past compile.
Local standalone testing wants a *loose cook* (fast, no paks). Only a build you intend
to **ship, deploy, or hand to a tester** justifies the full UAT package — it is far
slower and produces immutable paks.

---

## The target matrix

A UE project's `Source/*.Target.cs` files declare its build targets. Names are
project-specific — **read them, don't assume**. The canonical three:

| Target kind | `TargetType` in `*.Target.cs` | Typical name | Binary produced | Used for |
|---|---|---|---|---|
| **Editor** | `Editor` | `<Project>Editor` | engine's `UnrealEditor.exe` loads the module DLLs | PIE, all editor/MCP work |
| **Game / Client** | `Game` (or `Client`) | `<Project>` | `<Project>.exe` | standalone client, packaged build |
| **Server** | `Server` | `<Project>Server` or a custom name (e.g. `Dedicated`) | `<Project>Server.exe` / Linux ELF | dedicated/headless server |

The **wire/target name** passed to UBT is the `*.Target.cs` filename minus `.Target.cs`
— it is **not** necessarily the project name. The server target especially is often
renamed. To learn the real names, glob `Source/*.Target.cs` and read each file's
`TargetType` and the `…Target : TargetRules` constructor — that's the source of truth.

---

## The build flavors (the four things people actually ask for)

Each flavor is a point in the matrix. Resolve engine + project + target names first
(see "Resolving the inputs"), then run the canonical command. The commands below are
written generically; substitute the resolved `$engine`, `$uproject`, and target name.

### Flavor A — Editor, for local PIE

The default "build the game so I can play it / drive it with the MCP." Compiles the
**Editor** target (project module + every enabled plugin, including this repo's
UnrealMCP). No cook, no package — the editor cooks on demand.

**This repo already scripts it** — prefer the script over a raw UBT line:

```bash
scripts/build-editor.ps1                 # builds <Project>Editor, Development, Win64
scripts/build-editor.ps1 -Configuration DebugGame
```

Underneath, that is just UBT:

```
<engine>/Engine/Build/BatchFiles/Build.bat <Project>Editor Win64 Development -Project=<uproject> -WaitMutex
```

Then launch (also scripted): `scripts/launch-editor.ps1` (GUI) or `-Headless` for
unattended MCP driving. PIE itself is started *inside* the running editor (the `pie_*`
MCP tools), not by a build command.

### Flavor B — Standalone Windows client, local (Development)

A runnable client outside the editor, for fast iteration. **Compile + loose cook** —
no paks, content stays as loose files the exe reads directly.

```
# 1. compile the Game target
<engine>/Engine/Build/BatchFiles/Build.bat <Project> Win64 Development -Project=<uproject> -WaitMutex

# 2. loose cook for the client platform
<engine>/Engine/Build/BatchFiles/RunUAT.bat BuildCookRun -project=<uproject> \
    -platform=Win64 -clientconfig=Development -cook -skipstage -nocompile -nop4

# 3. run it
<project-dir>/Binaries/Win64/<Project>.exe <uproject> /Game/Maps/<EntryMap>
```

Loose cooks are incremental and fast; this is the iteration loop, not a deliverable.

### Flavor C — Packaged Windows client (Shipping / distributable)

A self-contained, paked package you can zip and hand to someone. Full UAT
`BuildCookRun`:

```
<engine>/Engine/Build/BatchFiles/RunUAT.bat BuildCookRun \
    -project=<uproject> \
    -platform=Win64 -clientconfig=Shipping \
    -build -cook -stage -pak -iostore -compressed \
    -prereqs -nodebuginfo \
    -archive -archivedirectory=<out-dir> \
    -AdditionalCookerOptions="-DisablePlugins=UnrealMCP" \
    -unattended -utf8output -nop4
```

Output is a standalone tree (the exe, `Engine/` runtime, and cooked paks) under
`<out-dir>`. **`-DisablePlugins=UnrealMCP`** keeps the editor-only MCP bridge out of a
shipping build — see Constraints.

### Flavor D — Dedicated server (local dev, then remote/cloud)

A headless server binary. Two sub-flavors — **don't conflate them**:

**D1 — local dev server** (Windows, Development, loose cook) for testing networking on
your own machine:

```
# compile the Server target (use its REAL name from *.Target.cs)
<engine>/Engine/Build/BatchFiles/Build.bat <ServerTarget> Win64 Development -Project=<uproject> -WaitMutex
# cook for the SERVER platform (note: WindowsServer, not Windows)
<engine>/Engine/Build/BatchFiles/RunUAT.bat BuildCookRun -project=<uproject> \
    -serverplatform=Win64 -server -serverconfig=Development -noclient -cook -skipstage -nop4
# run headless
<project-dir>/Binaries/Win64/<ServerTarget>.exe <uproject> /Game/Maps/<EntryMap> -log -port=7777
```

**D2 — deployable server** (cooked, paked, usually Linux for cloud hosting) — the
artifact you ship to a fleet:

```
<engine>/Engine/Build/BatchFiles/RunUAT.bat BuildCookRun \
    -project=<uproject> \
    -platform=Linux -server -serverplatform=Linux -serverconfig=Shipping -noclient \
    -build -cook -stage -pak -iostore -compressed -nodebuginfo \
    -archive -archivedirectory=<out-dir> \
    -AdditionalCookerOptions="-DisablePlugins=UnrealMCP" \
    -unattended -utf8output -nop4
```

A Linux server build **cross-compiles** — it needs the Linux toolchain installed and
`LINUX_MULTIARCH_ROOT` set (see Constraints). The output tree (Linux ELF + cooked paks
+ a launcher script) is what gets zipped, uploaded, and turned into a fleet.

**The actual remote/cloud deploy** — zip → upload to object storage → register a
build → create/scale a fleet → wire it to a session queue — is **hosting-platform
work, out of scope here.** This skill produces the *deployable server artifact*; for
the deployment itself defer to the **`/gamelift`** skill (hosting model, queues,
fleets) and **`/networking`** (replication). Keep the boundary clean: build here, host
there.

---

## Resolving the inputs (do this before any command)

1. **Engine** — `UNREAL_ENGINE_ROOT` (the directory containing `Engine/`). Required.
   If unset, run the **`/onboard`** skill (Part 1) — don't guess a path. Validate
   `Engine/Build/BatchFiles/Build.bat` and `RunUAT.bat` exist.
2. **Project** — `UNREAL_PROJECT_ROOT` (or a `-Project` path); resolve to the single
   `.uproject`. The repo scripts already do this resolution — reuse their logic.
3. **Target names** — glob `<project>/Source/*.Target.cs`, read each `TargetType`, and
   map Editor / Game / Server to their real names. **Never assume the server target is
   `<Project>Server`** — many projects rename it.
4. **Entry map** (flavors B/C/D) — the map to boot / the cook entry point. Check the
   project's `Config/DefaultEngine.ini` (`[/Script/EngineSettings.GameMapsSettings]`
   and `[/Script/UnrealEd.ProjectPackagingSettings] +MapsToCook`). Ask if ambiguous.

---

## Constraints and invariants (these silently break builds — respect them)

- **A Server target requires a SOURCE engine build, not a Launcher install.** Launcher
  (Epic Games Store) engines ship no `Server` target support and no `RunUBT`/full UAT
  for server cooks. If `UNREAL_ENGINE_ROOT` is an `installed` kind (an
  `Engine/Build/InstalledBuild.txt` exists) and the user wants a server build, **stop
  and say so** — they need a source build of the matching version.

- **The engine that COOKS content and the engine that LOADS it must be the same
  version.** Mismatched versions produce serialization failures at load, not at build.
  Don't cook with one engine and run a binary built against another.

- **Client and server cook to DIFFERENT platforms** (`Win64`/`Windows` vs
  `WindowsServer`, or `Linux` vs `LinuxServer`). Server content has rendering data
  stripped. Cooking the wrong platform for a target yields a build that's bloated
  (server with render assets) or missing data at runtime. Match `-serverplatform` to
  the server, `-platform`/`-clientconfig` to the client.

- **Editor-only plugins must be excluded from cooked/shipping builds.** This repo's
  `UnrealMCP` is an Editor-module plugin — pass
  `-AdditionalCookerOptions="-DisablePlugins=UnrealMCP"` to every package/cook of a
  client or server. Leaving it in pollutes the pak and can fail the cook.

- **Cooking is tree-shaken from entry-point maps.** `+MapsToCook` (in
  `ProjectPackagingSettings`) are the cook roots; anything not reachable from them (or
  in `+DirectoriesToAlwaysCook`) is excluded. A "missing asset at runtime" in a
  packaged build is almost always an asset that wasn't reachable from a cooked map.

- **Loose cook ≠ packaged.** A loose cook (`-skipstage`) is for local iteration only —
  it is not portable and has no paks. Never hand a loose cook to a tester or a fleet.

- **Linux server builds cross-compile** — they need the UE Linux toolchain (clang) and
  `LINUX_MULTIARCH_ROOT` pointing at it. Without it, the Linux build fails immediately
  at the toolchain probe. If targeting Linux from Windows and the var is unset, flag it
  before launching the (long) build.

- **Builds are slow and must not be left unbounded in this harness.** UAT packages take
  many minutes. Prefer `run_in_background` for the long flavors (C/D2) and report when
  done; never block an interactive turn on a full package. Compiles (A/B) are faster but
  still pass `-WaitMutex` so concurrent UBT invocations queue rather than collide.

---

## How to advise / act (the procedure)

1. **Classify the request into a flavor (A–D).** If the user just says "build the
   game," the default is **A (editor, for PIE)** — that's what unblocks driving the
   project with the MCP. Confirm the flavor when the ask is ambiguous (e.g. "build for
   Windows" could be B or C — ask dev-iteration vs distributable). Use
   `AskUserQuestion` only for a genuine fork, not to nag.
2. **Resolve inputs** (engine, project, target names, entry map) per the section above.
   Fail loudly and early if the engine is missing or the wrong kind for the flavor.
3. **Prefer the repo's scripts where they exist** (`build-editor`, `launch-editor`).
   For flavors with no script yet (B/C/D), run the canonical UBT/UAT command directly,
   substituting resolved values. If the user will repeat a flavor often, offer to
   capture it as a small `scripts/` wrapper (mirroring `build-editor.ps1`'s
   env-var-driven, project-generic shape) — don't hardcode the project into it.
4. **Run long builds in the background**, surface the log path, and report
   success/failure with the exit code and the last error lines on failure.
5. **Verify the artifact**, don't just trust exit 0: editor build → the editor binary /
   module DLLs exist and are newer; package → the expected exe/paks exist under the
   archive dir. Report the output path.
6. **Stop at the artifact for remote flavors.** Producing the deployable server build is
   this skill's finish line; hand deployment to `/gamelift` + `/networking`.

---

## Status / honesty

- Scripted in this repo today: **Flavor A** (`scripts/build-editor.{ps1,sh}`) and
  launching (`scripts/launch-editor`). Flavors **B/C/D** have **no wrapper script yet** —
  this skill carries their canonical commands and runs them directly; offer to script a
  flavor if it'll recur.
- Platform shown is Windows-host (`Build.bat`/`RunUAT.bat`). On macOS/Linux the
  equivalents are `Build.sh`/`RunUAT.sh` under the same `Engine/Build/BatchFiles/`;
  the flag sets are identical.
- The exact UAT flag set is a sensible, widely-used baseline, not the only valid one.
  Projects tune it (compression, encryption, chunking, `-manifests`). Treat the lines
  here as the contract's *shape*; read the project's `ProjectPackagingSettings` for its
  specific choices before a distributable build.
