---
name: onboard
description: Get this repo configured to run on the user's machine. Use when someone has just cloned/opened the Unreal Engine MCP repo and wants to set it up, "onboard", get started, or install its dependencies. Covers three things — (0) a dependency survey + install walkthrough (minimal vs full vs custom profile; C++ toolchain, uv/tests, Neo4j/Docker, GIMP, Pulumi/AWS, API keys), (1) locating the user's Unreal Engine install (UNREAL_ENGINE_ROOT), and (2) wiring this repo's plugin into the user's UE project so the MCP drives this code, plus setting UNREAL_PROJECT_ROOT. Invoke on requests like "onboard me", "set this up", "help me get configured", "install the dependencies", "where's my engine", "wire up the plugin".
---

# Onboard

Get this repo runnable on the user's machine. Onboarding has **one dependency phase**,
**two required parts**, plus **two optional parts**:

0. **Dependencies** — survey what the machine has, ask which profile the user wants
   (**Minimal** = core loop only / **Full** = everything / **Custom** = pick features),
   and walk the missing installs from proper sources. Matrix: `DEPENDENCIES.md`
   (same directory as this skill).
1. **Engine location** — find the Unreal Engine install and record `UNREAL_ENGINE_ROOT`.
2. **Plugin wiring** — point the user's UE project at *this repo's* plugin so the
   editor loads our C++ from here, and record `UNREAL_PROJECT_ROOT`.
3. *(Optional)* **OpenAI key** — for AI art generation (the `/icon` skill;
   AI-generated keycaps for `/key-indicator-helper`). Offer it once; skippable.
4. *(Optional)* **Google AI Studio (Gemini) key** — for video analysis
   (`video_analyze` / `pie_analyze`) and the visual-judgment skill
   (`/visual-critique`). Offer it once; skippable.

**If the user has no project yet, use the `bootstrap` skill instead of Part 2.**
Bootstrap scaffolds a fresh C++ project under `projects/`, enables the recommended
plugins, wires `AdditionalPluginDirectories`, builds it, and sets
`UNREAL_PROJECT_ROOT` — i.e. it does Part 2 for a new project. Part 1 (engine
location) is still a prerequisite for it. Run Part 1 here, then hand off to
`bootstrap`. Use Part 2 below only to wire an **existing** project.

Related helpers the agent can run once configured: `scripts/build-editor`,
`scripts/launch-editor` (`-Headless` for unattended MCP driving),
`scripts/stop-editor`, and `scripts/install-plugins` (fetch non-bundled
third-party plugins). All read the two env vars this skill sets.

**Source-of-truth rule (non-negotiable):** this repo is the one canonical copy of
the plugin (`src/Plugin/UnrealMCP/`). **Never copy, vendor, or `git submodule` the
plugin into the user's project, and never create a junction/symlink under their
`Plugins/` folder.** Wiring is done purely by pointing the project's `.uproject` at
this repo via `AdditionalPluginDirectories`. If the user already has a copied or
linked `Plugins/UnrealMCP` from an older setup, flag it and offer to remove it so
there's exactly one source.

Out of scope: configuring the user's MCP client (that's `README.md` → "Run the
server"). If asked, point them there and stop.

---

## Part 0 — Dependencies (profile + install walkthrough)

Goal: the user picks a setup profile; every dependency that profile needs is either
verified present, installed **with their consent from a source they chose**, or
consciously skipped (with the disabled feature named). The full matrix — what each
dependency is for, its detection command, and per-platform install sources — lives in
**`DEPENDENCIES.md` next to this file. Read it before running this part.**

The tiers, in one line each:

- **Baseline** (assumed; verify only): Bun ≥ 1.3, an Unreal Engine install, git.
- **Tier 1 — essential, every profile:** a C++ toolchain UBT accepts (Windows: VS 2022
  "Game development with C++" workload — correct MSVC band, .NET Framework 4.8 SDK,
  Windows SDK; macOS: Xcode; Linux: the source engine's bundled clang), plus
  `bun install` at the repo root.
- **Tier 2 — test suite:** `uv` (provides Python/pytest ephemerally for `tests/run.*`).
- **Tier 3 — per-feature:** Gemini key (video analysis + `/visual-critique`) · OpenAI
  key + Python art packages (`/icon`) · Python + Pillow/numpy (`/see`, `/texture`) ·
  GIMP 3 + a client-configured GIMP MCP server (`/gimp-import`) · Docker + Neo4j 5
  (`/neo4j`) · Pulumi + AWS CLI (deploy/hosting).

### 0.1 Survey (read-only, no installs)

Run the detection commands from `DEPENDENCIES.md` for everything — they're cheap, so
survey all tiers regardless of profile, in parallel. Summarize present/missing in a
short table before asking anything.

### 0.2 Ask the profile

`AskUserQuestion`: **Minimal (Recommended to start)** / **Full** / **Custom**. If
Custom, follow up with a multi-select over the Tier 2/3 features (tests, video+critique,
AI art, image tooling, GIMP pipeline, Neo4j graphs, deploy toolchain). Tier 1 is in
every profile — it is not offered as a choice, only sequenced.

### 0.3 Walk the missing items

For what the profile needs and the survey didn't find:

- Present the plan first: each missing item, the feature it unlocks, and the exact
  source you propose (official installer vs package manager — the user picks; either
  answer is fine, so is "skip").
- **Scriptable installs** (uv, Python packages, Pulumi, AWS CLI, `bun install`): run
  them after one batch confirmation, then re-verify each with its detection command.
- **User-action items** (Visual Studio workload, Docker Desktop, GIMP, API keys, AWS
  credentials): hand over exact steps + the official link, wait for "done", re-verify.
  **Never script Visual Studio component changes.**
- API keys are configured by **Parts 3/4** below — if the profile includes those
  features, run those parts; otherwise skip them entirely.

### 0.4 Record the outcome

Note anything skipped and the feature that stays off (this feeds the final summary in
step 10). `/onboard` can be re-run any time to add a tier — nothing here is one-shot.

---

## Part 1 — Engine location

Goal: a validated absolute path to the engine root (the directory containing
`Engine/`), persisted as `UNREAL_ENGINE_ROOT`. That variable is what the MCP
server (`editor_build_game_target`) and the test harness (`tests/harness/config.py`)
read to find the engine.

### 1. Discover candidates

Run the discovery helper — it scans the registry, conventional install locations,
and sibling projects, then prints ranked candidates as JSON. Read-only.

```powershell
pwsh -NoProfile -File scripts/find-engine.ps1
```

Each candidate has: `path`, `version`, `kind` (`source` = built from GitHub
source; `installed` = Epic Games Launcher build), `hasEditor` (is the editor
binary actually compiled), and `sources` (how it was found). Pre-ranked best-first.

On macOS/Linux there is no PowerShell discovery script yet — fall back to step 3
and validate with step 4.

### 2. Pick the candidate to propose

- **Prefer a `source` build with `hasEditor: true`.** This project is developed
  against an Unreal Engine *source* build, so a source build is the right default
  when both kinds are present.
- If `UNREAL_ENGINE_ROOT` is already set and matches a discovered candidate
  (`sources` includes `env`), say it's already configured and confirm the path is
  what they want — don't blindly re-write it.
- If exactly one good candidate exists, propose it.
- If several exist, present them (path + version + kind) and ask which with
  `AskUserQuestion`. Lead with the recommended source build.
- If the JSON is empty, go to step 3.

Always show the path you intend to use and get explicit confirmation before
recording.

### 3. If nothing was found

Ask the user where Unreal Engine is installed (the folder containing `Engine/`).
Typical spots: a GitHub source build under something like `C:\UE5\UnrealEngine-*`,
or a Launcher install under `C:\Program Files\Epic Games\UE_5.x`. Then validate
(step 4). If they don't have the engine, tell them they need to install or build
it first — that's a prerequisite, not something onboarding does.

### 4. Validate before recording

Whatever path you land on, confirm:

- `<path>/Engine/` exists.
- `<path>/Engine/Build/Build.version` exists (read it to confirm the version).
- Ideally `<path>/Engine/Binaries/Win64/UnrealEditor.exe` exists — if not, the
  engine isn't built yet; flag this, since the server can locate it but can't
  build a target against an uncompiled engine.

If the path fails the first two checks, do not record it — go back to step 3.

### 5. Record it

Persist `UNREAL_ENGINE_ROOT` so future shells (and the run/test scripts) see it.

**Windows** — set it for both the current session and persistently:

```powershell
$env:UNREAL_ENGINE_ROOT = '<confirmed-path>'   # this session
setx UNREAL_ENGINE_ROOT '<confirmed-path>'      # new shells (takes effect next launch)
```

**macOS/Linux** — set it for the session and add the export to their shell
profile (`~/.bashrc`, `~/.zshrc`, etc.):

```bash
export UNREAL_ENGINE_ROOT='<confirmed-path>'
```

`setx` only affects newly-launched shells, so a running `run-server` process must
be restarted to pick it up. (Background/non-interactive shells spawned *before*
the `setx` won't see it either — pass the value inline when invoking build scripts
from such a shell rather than relying on the persisted var.)

### 5b. Note how the engine is registered (feeds the project's `EngineAssociation`)

The way the engine is *registered* — not just its version — decides the
`EngineAssociation` a `.uproject` must carry, and a mismatch makes the editor
**refuse to open the project** ("made for a different engine version"). A **source
/ custom build** registers a `{GUID} = <engine path>` under
`HKCU\Software\Epic Games\Unreal Engine\Builds`; a **Launcher install** uses the
version string (`"5.7"`). The `bootstrap` skill resolves this when it writes the
`.uproject` (Step 2.1 there) — but note that `find-engine.ps1`'s `kind` can read
`installed` even for a registry-GUID build, so the **Builds registry key, not the
label, is authoritative.** When wiring an *existing* project (Part 2), check its
`EngineAssociation` matches the registered identity for `UNREAL_ENGINE_ROOT`.

---

## Part 2 — Wire the plugin into the user's project

Goal: the user's UE project loads the plugin **from this repo** (no copy), and
`UNREAL_PROJECT_ROOT` points at that project so the project-coupled server tools
(`editor_read_logs`, `editor_build_game_target`) work.

### 6. Identify the target project

This is the `.uproject` the user will drive with the MCP.

- Ask which project, unless it's obvious. Accept an absolute path to either the
  `.uproject` file or the directory containing it.
- A project may live **inside this repo** (e.g. `projects/<Name>/`) or anywhere
  external — both are fine.
- If the user doesn't have a project yet, **stop and use the `bootstrap` skill** —
  it scaffolds a C++ project, enables the recommended plugins, wires the plugin,
  builds it, and sets `UNREAL_PROJECT_ROOT`, replacing the rest of Part 2. Come
  back here only for an existing project.

Resolve the project directory (the folder containing the single `.uproject`) and
confirm it with the user before editing.

### 7. Compute the plugin path

The plugin's parent directory in this repo is what `AdditionalPluginDirectories`
must point at — **the parent `src/Plugin`, not `src/Plugin/UnrealMCP`** (UE scans
subdirectories for `.uplugin` files).

Get this repo's root reliably and build the path:

```powershell
$repo = (git rev-parse --show-toplevel)        # this repo's root
$pluginDir = Join-Path $repo 'src/Plugin'      # the value to register
```

Choose the form to write into the `.uproject`:

- **Project lives under this repo** (e.g. `<repo>/projects/Foo/`): write a path
  **relative to the `.uproject`**, e.g. `../../src/Plugin`. This keeps the whole
  tree portable if it moves as a unit.
- **External project**: write the **absolute** path to `<repo>/src/Plugin`.

Use forward slashes in the JSON (valid and cross-platform); avoid Windows
backslashes, which must be escaped in JSON and break on other platforms.

### 8. Edit the `.uproject`

Read the `.uproject` (it's JSON) and merge in two keys without clobbering
anything that's already there:

1. **`Plugins`** — ensure an entry `{ "Name": "UnrealMCP", "Enabled": true }`
   exists. If the array already has it, leave it. If the array is missing, create
   it.
2. **`AdditionalPluginDirectories`** — ensure the plugin path from step 7 is
   present. If the array exists, append the path only if it's not already there
   (don't duplicate). If missing, create it.

Keep the file valid JSON and preserve existing formatting/fields as much as
possible. Re-read it after editing to confirm it still parses.

**C++ caveat — check and warn, don't auto-fix:** the plugin ships C++, so the
host project must itself be a C++ project (a `Source/` directory with a module and
a `*.Target.cs`) for the plugin to compile. If the project has no `Source/`, tell
the user it's Blueprint-only and the plugin won't build until it has at least one
C++ module — offer to help add a minimal module, but don't silently convert it.

After editing, rebuild so the C++ plugin is compiled in — run
`scripts/build-editor.ps1` (it reads `UNREAL_ENGINE_ROOT` + `UNREAL_PROJECT_ROOT`;
pass `-Project <dir>` if the env var isn't set yet), or just open the project in
the editor, which will prompt to rebuild. The plugin then loads directly from this
repo.

**Toolchain caveat (Windows):** the build needs two pieces that a partial VS install
can lack, each failing *before* any code compiles (so they look like project faults
but aren't):
1. **An MSVC toolset the engine accepts.** UBT bans specific bands per release — UE
   5.7 rejects `14.40`–`14.43` and wants `14.44.35207` ("MSVC v143 — v14.44-17.14").
   Only-banned → `Result: Failed (OtherCompilationError)`, no compiler output.
2. **The .NET Framework SDK 4.6+** (4.8 SDK + targeting pack) for UE's
   `SwarmInterface` — missing → `Could not find NetFxSDK install dir … Result: Failed
   (RulesError)`.

Check: `vswhere -latest -property installationPath` → `VC\Tools\MSVC` for toolsets,
and `Test-Path "HKLM:\SOFTWARE\Microsoft\Microsoft SDKs\NETFXSDK"` for the .NET SDK.
Both ship with VS's **"Game development with C++"** workload; if either is missing
the user must add it via the Visual Studio Installer (a user action — don't
auto-install it).

### 9. Record `UNREAL_PROJECT_ROOT`

Persist the project directory (the folder containing the `.uproject`) so the
project-coupled server tools can find it.

**Windows:**

```powershell
$env:UNREAL_PROJECT_ROOT = '<project-dir>'     # this session
setx UNREAL_PROJECT_ROOT '<project-dir>'        # new shells
```

**macOS/Linux:**

```bash
export UNREAL_PROJECT_ROOT='<project-dir>'
```

As with the engine var, a running `run-server` must be restarted to pick it up.

---

## Part 3 (optional) — OpenAI key for AI art generation

Goal: a stored OpenAI API key so the host-side art-generation skills work. **This is
optional and only matters for AI image generation** — the `/icon` skill (generate → import
→ wire UI icons) and AI-generated keycaps for `/key-indicator-helper`. The editor, the MCP
server, and everything else need *nothing* here. Skipping this is fine; the user is
re-prompted automatically the first time they actually try to generate art (a `PreToolUse`
hook gates the generation command — `.claude/hooks/openai-cred-gate.py`), so you won't nag
them every session.

### 11. Offer it (once)

Ask whether they want to set up AI art generation now (`AskUserQuestion`): **Set up now** /
**Skip** (they can do it later, and will be prompted when they first generate). If a key is
already configured, say so and move on — `scripts/openai-key.sh check` reports it.

If they want to set it up, get their key and store it **without echoing it into the
transcript** — pipe it straight into the shared helper (the same command the generation-gate
hook tells them to run, so the process is identical wherever it's triggered):

```bash
printf %s '<their-OpenAI-key>' | scripts/openai-key.sh set
```

This writes `OPENAI_API_KEY` to a **gitignored `.env`** at the repo root; `iconify.py`
auto-loads it (python-dotenv) with no shell restart. Prefer having the **user run that
command themselves** (so the key never transits the assistant) — or, if they'd rather, accept
the key and run it for them. Confirm with `scripts/openai-key.sh check` (exit 0 = configured).

> Don't have a key / don't want AI art? That's a valid choice — skip. Art can always be
> supplied another way (existing PNGs + `--skip-gen`, hand-authored assets); only *generation*
> needs the key.

---

## Part 4 (optional) — Google AI Studio (Gemini) key for video analysis + visual judgment

Goal: a stored Google AI Studio (Gemini) API key. **Optional and opt-in** — one key powers
two features: the server's **video analysis** tools (`video_analyze` / `pie_analyze` — the
server reads `GEMINI_API_KEY` or `GOOGLE_STUDIO_API_KEY` and returns `feature_disabled`
without one; recording itself needs no key) and the **visual-judgment** skill
(`/visual-critique` — grade a render against a reference image OR a written spec). The
editor, the rest of the MCP server, and everything else need *nothing* here. Unlike AI art
there's no offline mode — these fundamentally call the Gemini API — so without a key
they're simply unavailable. Skipping is fine; the user is re-prompted automatically the first time they
actually run one (a `PreToolUse` hook gates the command — `.claude/hooks/gemini-cred-gate.py`),
so you won't nag them every session.

### 12. Offer it (once)

Ask whether they want to enable the visual-judgment skills now (`AskUserQuestion`): **Set up
now** / **Skip** (they can do it later, and will be prompted when they first run one). If a key
is already configured, say so and move on — `scripts/google-key.sh check` reports it.

If they want it, get their key (from <https://aistudio.google.com/apikey>) and store it
**without echoing it into the transcript** — pipe it straight into the shared helper (the same
command the gate hook tells them to run, so the process is identical wherever it's triggered):

```bash
printf %s '<their-Google-AI-Studio-key>' | scripts/google-key.sh set
```

This writes `GOOGLE_STUDIO_API_KEY` to the **gitignored `.env`** at the repo root;
`critique.ts` picks it up automatically (Bun auto-loads `.env`, and the script
also walks parent dirs for it) with no shell restart. Prefer having the **user run that command
themselves** (so the key never transits the assistant) — or, if they'd rather, accept the key
and run it for them. Confirm with `scripts/google-key.sh check` (exit 0 = configured).

> Don't have a key / don't want the visual critics? That's a valid choice — skip. You can
> always judge a render yourself or have the user eyeball it; only the automated
> reference/spec scoring needs the key.

---

## 10. Confirm done

Summarize:

- Dependencies: the chosen profile (Minimal/Full/Custom), what was verified, what was
  installed (and from where), and what was skipped — naming each feature a skip leaves
  off, with the note that re-running `/onboard` adds it later.
- Engine: final path, version, and kind; `UNREAL_ENGINE_ROOT` now points at it.
- Project: which `.uproject` was wired, the `AdditionalPluginDirectories` value
  added, and that `UNREAL_PROJECT_ROOT` now points at the project dir.
- AI art: whether an OpenAI key was configured (Part 3) or skipped — if skipped, note
  they'll be prompted automatically the first time they generate art.
- Gemini: whether a Google AI Studio key was configured (Part 4) or skipped — if
  skipped, note video analysis (`video_analyze`/`pie_analyze`) and `/visual-critique`
  are unavailable until set, and they'll be prompted automatically the first time they
  run the critic.
- Remind them the plugin loads from this repo (no copy) — edits to the C++ here
  show up on the next build.
- Point them at the helper scripts now usable: `scripts/build-editor` to compile,
  `scripts/launch-editor` to open the editor (`-Headless` for unattended MCP
  driving), `scripts/stop-editor` to shut it down.
- Final step is starting the server and pointing their MCP client at it
  (`README.md` → "Run the server"), which onboarding deliberately leaves to them.
