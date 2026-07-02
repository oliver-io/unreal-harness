---
name: bootstrap
description: Create a new Unreal Engine project under projects/ wired to this repo's UnrealMCP plugin and pre-configured with the recommended engine built-in plugins (Niagara, Water, GeometryScripting, GAS, StateTree, animation, runtime mesh, etc.), and give the project its own agent-facing CLAUDE.md. Use when someone wants to "bootstrap a project", "make a new UE project", "scaffold a test/sandbox project", get a fresh project to drive with the MCP, or "write a CLAUDE.md for this game". Produces a minimal C++ host project that loads the plugin from this repo (no copy) and builds against the engine at UNREAL_ENGINE_ROOT. Asks the intended architecture (C++-first vs Blueprint-first, single- vs multiplayer) and writes a project CLAUDE.md tuned to the answers; can also adopt an existing project by exploring it and writing a CLAUDE.md that describes it.
---

# Bootstrap

Scaffold a fresh, buildable Unreal project under `projects/<Name>/` that loads
**this repo's** UnrealMCP plugin and enables the engine built-in plugins we
commonly drive through the MCP. The result is a project you can open, build, and
immediately point the MCP server at.

**Every project this skill touches gets its own `CLAUDE.md`** at its root — an
agent-facing brief that tells a future Claude what the game is, how it's
architected, and the rules it must follow while building it (above all, the
**language policy**: C++-first vs Blueprint-first). This is the project's own
instructions, separate from the harness's top-level `CLAUDE.md`; the harness one
describes the workbench, this one describes the game.

### Two modes

1. **Scaffold a new game** (the default flow, Steps 1–8) — generate the project
   tree, ask the architectural questions, build, and write a `CLAUDE.md` tuned to
   the answers. Use the **"good CLAUDE.md for an Unreal Engine game"** template.
2. **Adopt an existing project** — when pointed at a UE project that already
   exists (one previously bootstrapped here, or an external project being brought
   under the harness), **don't scaffold.** Explore it read-only and write a
   `CLAUDE.md` that describes it *as it actually is*. See **"Adopting an existing
   project"** below; skip Steps 1–6.

This is the consumer-facing sibling of the **test harness**
(`tests/fixtures/TestProject` + `tests/harness/editor.py`): the harness builds a
throwaway fixture and **junctions** the plugin into `Plugins/` for isolation;
bootstrap instead points the project's `.uproject` at the plugin via
`AdditionalPluginDirectories` — no copy, no junction, per the source-of-truth rule
(see the `onboard` skill and `README.md`). The file layout below is copied from the
proven test fixture, so it builds against the same engine without per-version edits.

## Prerequisites

- `UNREAL_ENGINE_ROOT` must point at the engine (the dir containing `Engine/`).
  If it isn't set, run the **`onboard`** skill first — bootstrap needs it to build.
- A C++ toolchain (Visual Studio on Windows) with an **MSVC toolset the engine
  accepts**. The first build is slow (minutes). UnrealBuildTool *bans* specific
  MSVC versions per engine release — e.g. UE 5.7 rejects the `14.40`–`14.43` band
  and wants `14.44.35207` (the "MSVC v143 — v14.44-17.14" component). A machine
  carrying **only** a banned toolset fails the build before compiling anything
  (`Result: Failed (OtherCompilationError)`, zero code emitted), which reads like a
  project problem but isn't. Verify it before scaffolding — see the toolset check
  in Step 5.
- The **.NET Framework SDK 4.6+** (in practice the "4.8 SDK" + "4.8 targeting pack").
  UE's `SwarmInterface` module needs it; without it the build fails at module
  instantiation (`Could not find NetFxSDK install dir … Install a version of .NET
  Framework SDK at 4.6.0 or higher`, `Result: Failed (RulesError)`) — again before
  any code compiles. Install via Visual Studio Installer (Individual components →
  ".NET Framework 4.8 SDK" + "…targeting pack").
- Both the MSVC toolset and the .NET Framework SDK are normally pulled by VS's
  **"Game development with C++"** workload — a partial / custom VS install is what
  exposes these as separate, mid-build failures.

## Steps

### 1. Get a project name

Ask for a name if not given. It is also the primary C++ module name, so it must be
a valid identifier: start with a letter, then letters/digits only, no spaces
(e.g. `Sandbox`, `MCPScratch`). Reject anything else.

Refuse to proceed if `projects/<Name>/` already exists — don't clobber. (To start
over, tear the old one down first; see **Teardown** below.) If a project already
exists and the user wants a `CLAUDE.md` for it, that's the **adopt** mode, not
this one — jump to **"Adopting an existing project"**.

### 1.5. Ask the architectural questions (drives the CLAUDE.md)

Before scaffolding, ask the user about the intended architecture — these answers
decide which `CLAUDE.md` the project gets, and they're hard to change later. Use
`AskUserQuestion`; capture whatever else the user volunteers about the game (genre,
scope, one-line pitch) for the CLAUDE.md header.

**Q1 — Language stance (the load-bearing one).** *"Should this be a C++-first game
or a Blueprint-first game?"*

- **C++-first** → the CLAUDE.md gets the **C++-first language policy** (Appendix),
  written as a seasoned UE C++ engineer's house rule, not absolutist dogma:
  **business logic, systems, and data live in C++; Blueprints stay at the content
  and orchestration layer.** The standing rule is *C++ wherever it's correct* —
  which for gameplay logic is almost always. C++ holds the architecture (base
  classes, components, subsystems, gameplay systems) exposed to Blueprint via
  `UFUNCTION`/`UPROPERTY`; Blueprint subclasses wire assets and designer-tunable
  defaults, and the **event graph stays a thin router that funnels engine events
  (BeginPlay, input, overlaps, timers) into C++ calls** — nodes are fine, *business
  logic* in the graph is the smell. Asset-graph domains (Animation Blueprints /
  AnimGraph, Material, Niagara, Control Rig) have no C++ path and are authored as
  assets, driven from C++-exposed parameters.
- **Blueprint-first** → the CLAUDE.md gets the **Blueprint-first language policy**:
  author primarily via the MCP `bp_*` tools; drop to C++ only for
  performance-critical/heavy-math/engine-integration pieces, exposed back to BP.

**Q2 — Networking.** *"Single-player or multiplayer?"* If **multiplayer**, the
CLAUDE.md notes a **server-authoritative** stance, points at the **`/networking`**
skill (Iris-by-default, server authority, prediction discipline) and **`/gamelift`**
for hosting, and flags that single-process PIE is not a multiplayer test. If
**single-player**, omit the networking section entirely.

Only ask what you can't infer; if the user already stated the stance, skip the
question and proceed. These two map directly onto template variants in the Appendix.

### 2. Generate the project files

Create the tree below under `projects/<Name>/`, substituting `<Name>` everywhere.
Templates are in the **Appendix**. Files:

```
projects/<Name>/
  <Name>.uproject
  CLAUDE.md                 written in Step 7 from the architecture answers
  .gitignore
  Config/DefaultEngine.ini
  Config/DefaultGame.ini
  Content/.gitkeep
  Source/<Name>.Target.cs
  Source/<Name>Editor.Target.cs
  Source/<Name>/<Name>.Build.cs
  Source/<Name>/<Name>.cpp
  Source/<Name>/<Name>.h
```

The `.uproject` carries: the primary game module, the recommended plugin list
(step 3), and `AdditionalPluginDirectories` pointing at this repo's plugin
(`../../src/Plugin`) plus the fetched-plugins dir (`../../external/Plugins`, for
GameLift) (step 4). All are in the Appendix template — write it verbatim with
`<Name>` and `<EngineAssociation>` (Step 2.1) substituted.

### 2.1. Resolve the `EngineAssociation` (do NOT hardcode the version)

`EngineAssociation` tells the editor *which* engine the project belongs to, and a
wrong value makes the editor **refuse to open the project** ("made for a different
engine version") and `Build.bat` resolve the wrong engine. What's valid depends on
how the engine is **registered**, not just its version number:

- **Epic Launcher install** → the version string, e.g. `"5.7"`.
- **Source / custom-registered build** (what this repo develops against) → a
  **GUID**, not a version. Custom builds register under
  `HKCU\Software\Epic Games\Unreal Engine\Builds` as `{GUID} = <engine path>`; use
  the GUID whose value resolves to the same path as `UNREAL_ENGINE_ROOT`.

⚠️ Don't trust `find-engine.ps1`'s `kind` label to decide this — it can report
`installed` for a build that is in fact registered by GUID. The **Builds registry
key is the source of truth.** Resolve it before writing the `.uproject` (Windows):

```powershell
$root = (Resolve-Path $env:UNREAL_ENGINE_ROOT).Path
$builds = Get-ItemProperty "HKCU:\Software\Epic Games\Unreal Engine\Builds" -ErrorAction SilentlyContinue
$assoc = $null
if ($builds) {
  foreach ($p in $builds.PSObject.Properties) {
    if ($p.Name -like '{*}' -and (Test-Path $p.Value) -and (Resolve-Path $p.Value).Path -eq $root) { $assoc = $p.Name; break }
  }
}
if (-not $assoc) {
  # Fallback: a Launcher install — use the "Major.Minor" version string (e.g. '5.7').
  $bv = Get-Content (Join-Path $root 'Engine\Build\Build.version') -Raw | ConvertFrom-Json
  $assoc = "$($bv.MajorVersion).$($bv.MinorVersion)"
}
$assoc   # ← substitute this for <EngineAssociation> in the template
```

If the lookup is empty *and* there's no Launcher install, opening the project once
and letting the editor repair the association also works — but resolving it up
front avoids the failed first open.

### 2.5. Fetch downloadable plugins

The template enables `GameLiftServerSDK`, which isn't engine-bundled. Fetch it
(and any other registered third-party plugins) into the repo's gitignored
`external/Plugins/`:

```powershell
scripts/install-plugins.ps1            # or install-plugins.sh
```

Per the **GameLift caveat** in step 3, this is `Server`-target-only: it doesn't
affect the Editor build bootstrap performs, and its native SDK build is only needed
if you later build a dedicated Server target. If the user doesn't want GameLift at
all, skip this step and remove the `GameLiftServerSDK` line + the
`../../external/Plugins` entry from the `.uproject`.

### 3. Recommended plugins (already in the template)

The `.uproject` template enables **engine built-in** plugins we exercise via the
MCP. Curated from a production UE reference project (proven to build against this
engine), minus any third-party/marketplace and opinionated entries:

- **Authoring / mesh / VFX:** `Niagara`, `GeometryScripting`,
  `ModelingToolsEditorMode` (Editor-only), `ProceduralMeshComponent`,
  `ChaosOutfitAsset`
- **World:** `Water`, `Landmass`, `CableComponent`
- **Gameplay / AI:** `GameplayAbilities`, `StateTree`, `GameplayStateTree`,
  `EnhancedInput`
- **Animation / rigging:** `ControlRig`, `IKRig`, `MotionWarping`,
  `AnimationWarping`
- **Audio / characters / networking:** `SteamAudio`, `MetaHumanCharacter`, `Iris`
- **Editor:** `EditorScriptingUtilities`
- **Hosting (fetched, not engine-bundled):** `GameLiftServerSDK` — see the
  GameLift caveat below
- **The MCP itself:** `UnrealMCP`

**Deliberately excluded** (don't add unless the user asks — paid marketplace
content with no public source to fetch; install via the Epic/Fab launcher):
`DragonIKPlugin`.

Some of these (`Niagara`, `EnhancedInput`, `IKRig`, `GeometryScripting`,
`GameplayAbilities`, `StateTree`, `GameplayStateTree`, `EditorScriptingUtilities`)
are also pulled transitively by `UnrealMCP.uplugin`'s own dependencies — listing
them explicitly is intentional so the project stays self-describing.

**GameLift caveat (important):** `GameLiftServerSDK` is **not** bundled with the
engine — it's fetched by `scripts/install-plugins` into the repo's
`external/Plugins/` (step 2.5), which is why the template points
`AdditionalPluginDirectories` at that dir.

It's gated with `"TargetAllowList": ["Server"]`, so it's enabled **only for
dedicated Server target builds** — excluded from Editor, Game, and Client targets,
and therefore **not loaded in PIE**. Because bootstrap builds the **Editor** target
(`<Name>Editor`), the normal editor/MCP workflow is unaffected: it builds clean
whether or not GameLift's native SDK is present. The native C++ Server SDK build
(per the [upstream README](https://github.com/amazon-gamelift/amazon-gamelift-plugin-unreal))
only matters when you actually build a **Server** target.

Two cautions: (1) fetching it (step 2.5) is still recommended so UBT can resolve
the descriptor referenced in the `.uproject`; (2) the upstream repo ships several
`.uplugin` files (server SDK + an **editor-UI** plugin + samples) — only
`GameLiftServerSDK` is referenced here; do **not** also enable the editor-UI
plugin, or you'd pull GameLift into the Editor build and hit the native-SDK
requirement. If the user wants none of this, drop the `GameLiftServerSDK` line (and
the `external/Plugins` entry) — everything else is engine-bundled and builds clean.

### 4. Confirm the plugin wiring

`AdditionalPluginDirectories` entries are relative to the `.uproject`:
`projects/<Name>/` → `../../` is the repo root. So `../../src/Plugin` is the
UnrealMCP plugin's parent and `../../external/Plugins` is the fetched-plugins dir
(UE scans both subtrees for `.uplugin` files). Point at the **parent** `src/Plugin`,
never at `src/Plugin/UnrealMCP` directly.

If the user is bootstrapping **outside** this repo's `projects/` for some reason,
use absolute paths to `<repo>/src/Plugin` and `<repo>/external/Plugins` instead.
Get the repo root with `git rev-parse --show-toplevel`.

### 5. Build the editor target

Building compiles the project module **and** the UnrealMCP plugin + its
dependencies — this is what makes the C++ loadable.

**First, confirm a non-banned MSVC toolset is installed** (see Prerequisites) — a
banned-only machine fails instantly with `OtherCompilationError` and no compiler
output, which is easy to misread as a project bug:

```powershell
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Select-Object -ExpandProperty Name   # MSVC toolsets
Test-Path "HKLM:\SOFTWARE\Microsoft\Microsoft SDKs\NETFXSDK"                                     # .NET Framework SDK present?
```

If the only MSVC version printed falls in the engine's banned range (e.g. `14.40.*`–
`14.43.*` for UE 5.7), or the NETFXSDK key is missing, **stop and tell the user to
install the missing piece** — the accepted MSVC toolset ("MSVC v143 — v14.44-17.14")
and/or the ".NET Framework 4.8 SDK" + targeting pack, via Visual Studio Installer →
Modify → Individual components (updating VS itself may be needed first). This is a
**user action** — don't try to install multi-GB VS components for them.

Then build with the generic build script (it resolves the engine + project from the
env vars / `-Project`):

```powershell
scripts/build-editor.ps1        # or build-editor.sh; builds <Name>Editor
```

If the wrapper errors before invoking the compiler (e.g. a project-resolution
failure in a non-interactive shell), fall back to calling `Build.bat` directly with
an explicit `-Project=<abs path to .uproject>` — that's all the wrapper does on its
last line.

Once `UNREAL_PROJECT_ROOT` is set (step 6) it needs no args; before that, pass
`-Project projects\<Name>`. Stream/inspect the output; a non-zero exit is a build
failure — surface it, don't claim success.

The build is what populates the gitignored `Binaries/` + `Intermediate/`. If the
user only wants the files scaffolded (no compile yet), skip this and tell them to
build, or to open the `.uproject` (the editor will prompt to compile).

### 6. Record the project root

Set `UNREAL_PROJECT_ROOT` so the project-coupled server tools (`editor_read_logs`,
build tools) target this project:

```powershell
$env:UNREAL_PROJECT_ROOT = '<abs path>\projects\<Name>'   # this session
setx UNREAL_PROJECT_ROOT '<abs path>\projects\<Name>'      # new shells
```

(macOS/Linux: `export` it and add to the shell profile.) A running `run-server`
must be restarted to pick it up.

### 7. Write the project's CLAUDE.md

Write `projects/<Name>/CLAUDE.md` from the **Appendix CLAUDE.md template**, assembled
from the Step 1.5 answers:

- Fill the **header/pitch** from whatever the user said about the game (genre, goal,
  one-liner). Don't invent a design — keep it to what you know; the `GDD.md` is where
  design lives.
- Drop in the **language-policy** block for the chosen stance (C++-first *or*
  Blueprint-first) — pick one, delete the other.
- Include the **networking** block only if the game is multiplayer; otherwise omit it.
- Keep the **harness relationship**, **authoring workflow**, **build/run**, and
  **doc convention** sections verbatim (they're true for every game here).

The C++-first variant should read like an experienced UE C++ engineer's philosophy,
not absolutist dogma: *C++ wherever it's correct — business logic, systems, and data
in C++; Blueprints at the content and orchestration layer.* Keep the nuance the user
called out: the **event graph is allowed to exist as a thin event-router that calls
into C++** — what doesn't belong there is the *business logic*, which lives in C++
functions the graph calls. Asset-graph domains (anim/material/Niagara/Control Rig)
are authored as assets because there's no C++ alternative. "C++ where correct," not
"never touch a Blueprint node."

### 8. Confirm done

Report: project path, that the plugin loads from this repo via
`AdditionalPluginDirectories` (no copy), build result, the chosen **language
stance** (so the user can confirm the CLAUDE.md policy is what they wanted), and
that `UNREAL_PROJECT_ROOT` now points at it. Next steps:

- Launch the editor: `scripts/launch-editor.ps1` (add `-Headless` for unattended
  MCP driving).
- Start the MCP server (`README.md` → "Run the server").

## Adopting an existing project

When pointed at a project that already exists — one bootstrapped here earlier, or an
external UE project being brought under the harness — **do not scaffold or build.**
Explore it read-only and write a `CLAUDE.md` at its root that orients a future agent
to the project *as it actually is*. Steps 1–6 don't apply; this replaces them.

**Explore first (read-only — never mutate the project here):**

- **`.uproject`** — modules, enabled plugins, engine version, and whether `UnrealMCP`
  is wired via `AdditionalPluginDirectories`. If it isn't wired, say so and offer to
  wire it (the Step 4 pattern) — agents can't drive it through the MCP until it is.
- **`Source/`** — module layout and primary classes. This is how you *infer the real
  language stance*: is gameplay implemented in C++ classes, or is `Source/` a thin
  host with the logic living in Blueprints under `Content/`? Report the stance the
  code actually follows — don't impose one.
- **`Content/`** — asset organization and the `/Game/<X>/` top-level folders, key
  Blueprints, maps. Use `asset_list` if an editor + MCP are live; otherwise the file
  tree.
- **`Config/`** — default/startup maps, game-mode override, input.
- **Existing docs** — `GDD.md` / `PLAN.md` / `STATUS.md` / `README`. Fold their gist
  into the CLAUDE.md and **link** them rather than duplicating; `STATUS.md` (if any)
  is the authoritative current state.
- **Live editor (if up)** — ground the description in real state with read tools:
  `project_context`, `scene_brief`, `bp_list_graphs`/`bp_inspect`, `level_inspect`.

**Then write `CLAUDE.md`** from the Appendix template, filled from what you found:
- Describe the game from its actual docs/code, not a guess.
- Use the language-policy variant that matches reality. **If the existing stance is
  ambiguous (mixed C++/Blueprint with no clear intent), ask the user which policy the
  project should follow going forward** before writing that section — adoption is also
  a chance to set direction, but don't silently pick one.
- Include the networking block only if the project is actually multiplayer.

Report what you found (stance, MCP wiring status, doc links) and where the CLAUDE.md
was written.

## Teardown

The project is cheap to delete — the whole `projects/` dir is gitignored, so
nothing is committed. To remove one: stop any editor using it
(`scripts/stop-editor.ps1`), then delete `projects/<Name>/`. To reset just the
build artifacts (force a clean rebuild), delete `Binaries/`, `Intermediate/`, and
`DerivedDataCache/` inside it.

---

## Appendix — file templates

Substitute `<Name>` (the project/module name) and `<EngineAssociation>` (Step 2.1)
throughout.

### `<Name>.uproject`

```json
{
	"FileVersion": 3,
	"EngineAssociation": "<EngineAssociation>",
	"Category": "",
	"Description": "MCP sandbox project. Loads the UnrealMCP plugin from this repo via AdditionalPluginDirectories (not copied); Binaries/Intermediate/Saved are gitignored and rebuilt on demand.",
	"Modules": [
		{
			"Name": "<Name>",
			"Type": "Runtime",
			"LoadingPhase": "Default"
		}
	],
	"Plugins": [
		{ "Name": "UnrealMCP", "Enabled": true },
		{ "Name": "EditorScriptingUtilities", "Enabled": true },
		{ "Name": "EnhancedInput", "Enabled": true },
		{ "Name": "Niagara", "Enabled": true },
		{ "Name": "GeometryScripting", "Enabled": true },
		{ "Name": "ProceduralMeshComponent", "Enabled": true },
		{ "Name": "ModelingToolsEditorMode", "Enabled": true, "TargetAllowList": [ "Editor" ] },
		{ "Name": "ChaosOutfitAsset", "Enabled": true },
		{ "Name": "Water", "Enabled": true },
		{ "Name": "Landmass", "Enabled": true },
		{ "Name": "CableComponent", "Enabled": true },
		{ "Name": "GameplayAbilities", "Enabled": true },
		{ "Name": "StateTree", "Enabled": true },
		{ "Name": "GameplayStateTree", "Enabled": true },
		{ "Name": "ControlRig", "Enabled": true },
		{ "Name": "IKRig", "Enabled": true },
		{ "Name": "MotionWarping", "Enabled": true },
		{ "Name": "AnimationWarping", "Enabled": true },
		{ "Name": "SteamAudio", "Enabled": true },
		{ "Name": "MetaHumanCharacter", "Enabled": true },
		{ "Name": "Iris", "Enabled": true },
		{ "Name": "GameLiftServerSDK", "Enabled": true, "TargetAllowList": [ "Server" ] }
	],
	"AdditionalPluginDirectories": [
		"../../src/Plugin",
		"../../external/Plugins"
	]
}
```

### `Source/<Name>.Target.cs`

```csharp
using UnrealBuildTool;
using System.Collections.Generic;

public class <Name>Target : TargetRules
{
	public <Name>Target(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("<Name>");
	}
}
```

### `Source/<Name>Editor.Target.cs`

```csharp
using UnrealBuildTool;
using System.Collections.Generic;

public class <Name>EditorTarget : TargetRules
{
	public <Name>EditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("<Name>");
	}
}
```

### `Source/<Name>/<Name>.Build.cs`

```csharp
using UnrealBuildTool;

// Empty primary game module — the project exists as a compilable host so the
// UnrealMCP editor plugin has a target to build into. Keep deps minimal; the
// plugins do the heavy lifting.
public class <Name> : ModuleRules
{
	public <Name>(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore" });
	}
}
```

### `Source/<Name>/<Name>.cpp`

```cpp
#include "<Name>.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, <Name>, "<Name>");
```

### `Source/<Name>/<Name>.h`

```cpp
#pragma once

#include "CoreMinimal.h"
```

### `Config/DefaultEngine.ini`

```ini
; Intentionally minimal. Blank startup/default maps make the editor open a fresh,
; unsaved "Untitled" transient level — a disposable scratch level. Nothing is
; saved unless you explicitly save an asset, so an editor restart is a clean reset.

[/Script/EngineSettings.GameMapsSettings]
EditorStartupMap=
GameDefaultMap=
```

### `Config/DefaultGame.ini`

```ini
[/Script/EngineSettings.GeneralProjectSettings]
ProjectName=<Name>
```

### `.gitignore`

```gitignore
# Generated/engine churn never gets committed.
Binaries/
Intermediate/
Saved/
DerivedDataCache/
Build/
*.sln
.vs/

# The UnrealMCP plugin loads from this repo via AdditionalPluginDirectories,
# so nothing is vendored under Plugins/. Ignore it in case the editor ever
# writes a local plugin here.
Plugins/
```

### `Content/.gitkeep`

Empty file (keeps the `Content/` dir tracked).

### `CLAUDE.md` (the project's agent brief)

Assemble this from the Step 1.5 answers: fill `<Name>` and the pitch, paste **one**
language-policy block (C++-first *or* Blueprint-first), and keep the networking block
only for multiplayer games. Everything else is verbatim — it's true for every game
under this harness. Keep it terse; deep design lives in `GDD.md`, current state in
`STATUS.md`.

````markdown
# CLAUDE.md — <Name>

Agent instructions for building **<Name>**. This is the *game*; the workbench that
drives it (MCP server, UnrealMCP plugin, skills, tests) lives in the harness repo at
`../../` — see `../../CLAUDE.md` for how the harness works. This file is the law for
*this game*.

## What this is

<One- to three-line pitch: genre, core loop, the thing that makes it this game.
Keep it factual — design detail belongs in GDD.md, not here.>

Authoritative current state is **`STATUS.md`** — read it first when resuming.
Design lives in **`GDD.md`**; the build log/plan in **`PLAN.md`**. Harness-level
bugs/findings cross-link to `../../docs/BUGS.md`.

## How it's wired

- Loads the **UnrealMCP** plugin **by reference** (`AdditionalPluginDirectories` →
  `../../src/Plugin`) — never copied. Edit the plugin in the harness, this project
  picks it up on the next build.
- This project is **gitignored from the harness** — it carries its own source control
  (or none). The harness never commits game content.
- Engine at `UNREAL_ENGINE_ROOT`; this project is `UNREAL_PROJECT_ROOT`.

## How you build it: through the MCP

You author this game by **driving a live Unreal Editor through the MCP tools**, not by
hand-editing assets. Blueprints via `bp_*`, materials via `material_*`, animation via
`anim_*`, Niagara via `niagara_*`, actors/levels via `actor_*`/`level_*`, and so on.
Inspect before you mutate (the read tools are deeper than the write tools). Prefer
`dry_run:true` to preview a mutator's diff when unsure.

<!-- ===================== PASTE ONE LANGUAGE POLICY ===================== -->

<!-- ---------- C++-FIRST VARIANT ---------- -->
## Language policy — C++ first

This is a **C++ game**, built the way an experienced Unreal C++ engineer builds one:
**business logic, systems, and data live in C++; Blueprints stay at the content and
orchestration layer.** The standing rule is **C++ wherever it's correct** — and for
gameplay logic, systems, state, and data, it's almost always correct. Default to a
C++ class for anything with behaviour.

**What belongs in C++ (the default for anything with logic):**
- Gameplay systems, mechanics, rules, state machines, calculations, data structures.
- Framework classes — GameMode, GameState, PlayerController, Pawn/Character,
  components, subsystems. These are C++ bases; Blueprints derive from them.
- Anything that should be reviewed, diffed, tested, reused, networked, or run hot.

**What Blueprints are for (the leaves and the wiring, not the logic):**
- **Content & tuning** — thin Blueprint subclasses of C++ classes that bind assets
  (mesh, material, input mappings) and expose designer-tunable defaults
  (`UPROPERTY(EditAnywhere/EditDefaultsOnly)`): the visual, iterable layer over a
  C++ skeleton.
- **The event graph as orchestration, not business logic.** It's expected and fine
  for a Blueprint to have an event graph that responds to engine events (BeginPlay,
  input actions, overlaps, timers, timelines) and **routes them into C++** — calling
  `BlueprintCallable` functions, reading `BlueprintReadOnly` state. Keep it a *thin
  router*: events in → call C++ → done. The business logic those events trigger —
  branchy rules, math, multi-step state changes — belongs in C++, and the graph
  calls it. **A sprawling logic-in-nodes event graph is the smell to fix, not the
  mere presence of nodes.**
- **Asset-graph systems with no C++ authoring path** — Animation Blueprints /
  AnimGraph, **Material** graphs, **Niagara**, **Control Rig**. These *are* assets;
  author them (via the MCP `anim_*` / `material_*` / `niagara_*` tools) and drive
  them from **C++-exposed parameters** wherever possible.
- **Integration glue** with UE or third-party systems that only accept Blueprints.

**Design the C++↔Blueprint seam deliberately:** expose C++ to the graph with
`UFUNCTION(BlueprintCallable)` and `UPROPERTY(EditAnywhere, BlueprintReadWrite)`; use
`BlueprintNativeEvent`/`BlueprintImplementableEvent` for the hooks a designer fills
in. The Blueprint holds the content and the wiring; the C++ holds the behaviour.

**Rule of thumb:** every class with behaviour is a C++ class (`AActor`, `ACharacter`,
`UActorComponent`, `U*Subsystem`, `AGameModeBase`, …) with a clean
`UFUNCTION`/`UPROPERTY` surface; its Blueprint child, if any, wires assets, sets
defaults, and orchestrates events into C++ calls. **When business logic starts
growing in an event graph, that's the signal to move it down into C++** — not because
Blueprints are forbidden, but because logic belongs where it can be reviewed, tested,
and reused.

Build C++ with `scripts/build-editor.ps1` (or `editor_live_coding_compile` for
hot-patch). Never claim a C++ change is live until it has compiled.

<!-- ---------- BLUEPRINT-FIRST VARIANT ---------- -->
## Language policy — Blueprint-first

This game is authored primarily in **Blueprints**, through the MCP `bp_*` tools. Prefer
Blueprints for gameplay logic, actors, and UI. **Drop to C++ only when a Blueprint
can't do it well** — performance-critical inner loops, heavy math, complex data
structures, engine integration needing native access, or a base class you want to
subclass widely. When you do write C++, **expose it back to Blueprints**
(`UFUNCTION(BlueprintCallable)`, `UPROPERTY(BlueprintReadWrite)`) so the BP layer stays
in control. Keep graphs small and readable; factor large graphs into functions or push
the core into a C++ base.

<!-- ===================== END LANGUAGE POLICY ===================== -->

<!-- ============ NETWORKING — keep only if multiplayer ============ -->
## Networking

This is a **multiplayer** game: **server-authoritative**. The server decides every
gameplay-meaningful change; clients request, predict their own pawn, and reconcile.
For the replication architecture (Iris by default; property vs RPC vs custom per kind
of state) use the **`/networking`** skill; for dedicated-server hosting and matchmaking
use **`/gamelift`**. **Single-process PIE is not a multiplayer test** — verify with a
real dedicated server + at least two clients.
<!-- ============ END NETWORKING ============ -->

## Conventions

- Game assets live under `/Game/<Name>/...`; keep a tidy folder taxonomy
  (`Blueprints/`, `Materials/`, `Maps/`, `Characters/`, …).
- Keep `STATUS.md` current — it's what the next session reads first.
- Anything that drives a live editor must be **bounded and self-cleaning** (timeouts +
  guaranteed `pie_stop`); never leak a PIE session. See `../../docs/TESTING.md`.

## Run

```
scripts/launch-editor.ps1        # open the editor (add -Headless for unattended MCP)
scripts/run-server.ps1           # start the MCP server
scripts/build-editor.ps1         # rebuild after C++ changes
```
````
