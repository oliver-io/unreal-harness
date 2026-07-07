# Dependency matrix — what a machine needs, by tier

Read by **Part 0** of the onboard skill. This is the rigorous inventory of everything the
harness can use, split into what is *essential* for the core MCP loop and what each
*optional* feature adds. Detection commands are read-only; install columns name the
**official source first** and package-manager alternatives second.

**Machine baseline (assumed, verify only):** Bun ≥ 1.3 and an Unreal Engine install
(source or Launcher). If the engine is missing, installing/building it is a prerequisite,
not an onboarding task — say so and stop. If Bun is missing, offer the official installer
(<https://bun.sh>: `curl -fsSL https://bun.sh/install | bash`, PowerShell
`irm bun.sh/install.ps1 | iex`, or `winget install Oven-sh.Bun` / `brew install oven-sh/bun/bun`).

## Profiles

| Profile | What works | Tiers installed |
| ------- | ---------- | --------------- |
| **Minimal** | The full core loop: MCP server, all ~260 editor tools, PIE, screenshots, in-engine video *recording* (Windows) | Baseline + Tier 1 |
| **Full** | Everything below: tests, video *analysis*, visual critique, AI art, image tooling, GIMP pipeline, Neo4j graphs, deploy toolchain | All tiers |
| **Custom** | Minimal plus the features the user picks | Baseline + Tier 1 + chosen Tier 3 items (offer Tier 2 too) |

Tier 1 is **never optional** — without a C++ toolchain the plugin can't compile, the
editor bridge never comes up, and no MCP tool works.

## Walkthrough rules (apply to every item)

1. **Detect first.** Run all detection commands for the chosen profile up front,
   read-only, in parallel. Show the user a short present/missing summary.
2. **Never install without consent.** Present what's missing, what each item is for, and
   the exact source you propose to install from; get one explicit confirmation for the
   scriptable batch. Users may prefer the official installer over a package manager —
   offer both and let them choose (or skip).
3. **User-action items are handed over, not automated:** the Visual Studio workload,
   Docker Desktop, GIMP (GUI installers, admin prompts, EULAs) and all **API keys**. Give
   the exact steps + official link, wait for them to say it's done, then re-verify.
   Never script Visual Studio component changes.
4. **Re-verify after every install** with the same detection command before moving on.
5. **Skipped ≠ failed.** Record each skip and name the feature that stays off; `/onboard`
   can be re-run any time to add a tier.

---

## Baseline — verify only

### git
- **Why:** cloning this repo, plugin wiring (`git rev-parse --show-toplevel`),
  `scripts/install-plugins` (clones third-party UE plugin sources), the key helpers
  (`scripts/*-key.sh` untrack `.env`).
- **Detect:** `git --version`
- **Install:** official <https://git-scm.com> · `winget install Git.Git` ·
  macOS: Xcode CLT (`xcode-select --install`) or `brew install git` · Linux: distro package.
- **Note (Windows):** Git for Windows also provides **Git Bash**, which the `.sh`
  helpers (`scripts/openai-key.sh`, `scripts/google-key.sh`, `scripts/neo4j.sh`) run under.

### Bun ≥ 1.3 / Unreal Engine
- **Detect:** `bun --version`; engine handled by Part 1 (`scripts/find-engine.ps1`).

---

## Tier 1 — essential (every profile)

### C++ build toolchain
The plugin is C++ and is compiled by UnrealBuildTool against the host project. Nothing
in the harness works until this builds.

- **Windows:** Visual Studio 2022 with the **"Game development with C++"** workload.
  That workload carries the three things UBT needs, each of which fails *before any code
  compiles* when missing (see the Toolchain caveat in Part 2, step 8, for the failure
  signatures):
  1. An **MSVC toolset in the band the engine accepts** — UE 5.7 rejects `14.40`–`14.43`
     and wants `14.44` ("MSVC v143 — v14.44-17.14").
  2. The **.NET Framework 4.8 SDK + targeting pack**.
  3. A **Windows 10/11 SDK** — this also supplies the Media Foundation libraries
     (`mfplat`/`mfreadwrite`/`mfuuid`) the plugin's `pie_record_*` video recorder links;
     no separate install ever needed for video recording.
  - **Detect:** `vswhere -latest -property installationPath` (vswhere lives at
    `${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe`), then list
    `<install>\VC\Tools\MSVC\*` for toolset versions, and
    `Test-Path "HKLM:\SOFTWARE\Microsoft\Microsoft SDKs\NETFXSDK"` for the .NET SDK.
  - **Install:** **user action** via the Visual Studio Installer
    (<https://visualstudio.microsoft.com>; Community is free). `winget install
    Microsoft.VisualStudio.2022.Community` can bootstrap it, but workload/component
    selection happens in the Installer GUI — walk the user through ticking the workload,
    never script it.
- **macOS:** Xcode (App Store) + command-line tools, in a version the engine release
  supports. **Detect:** `xcodebuild -version`.
- **Linux:** a source engine bundles its own clang toolchain (`Setup.sh` fetches it);
  normally nothing extra. **Detect:** the engine's `Engine/Extras/ThirdPartyNotUE` /
  toolchain dir exists after setup, or just attempt the build.

### Server packages
- **Why:** the MCP server's npm deps (`@modelcontextprotocol/sdk`, `zod`, `@google/genai`, …).
- **Install:** `bun install` at the repo root — no separate machine installs.
  (`plugin/unreal-skills/skills/neo4j/tool/` has its own `bun install`, run automatically by `scripts/neo4j.*` on first use.)

---

## Tier 2 — test suite (offer under Full/Custom; recommended for contributors)

### uv (Astral)
- **Why:** `tests/run.ps1` / `tests/run.sh` run the live-editor pytest suite via
  `uv run --with pytest --with "mcp[cli]>=1.10.0"` — an ephemeral env, so neither Python
  nor pytest needs to be pre-installed. (`bun test src/server` needs nothing extra.)
- **Detect:** `uv --version`
- **Install:** official Astral <https://docs.astral.sh/uv>:
  `curl -LsSf https://astral.sh/uv/install.sh | sh` · PowerShell
  `irm https://astral.sh/uv/install.ps1 | iex` · `winget install astral-sh.uv` ·
  `brew install uv`.
- **Alternative without uv:** Python 3.10+ and
  `pip install -r tests/requirements-test.txt`, then `python -m pytest` from `tests/`.

---

## Tier 3 — per-feature optional

### Google Gemini API key — video analysis + visual critique
- **Enables:** `video_analyze` / `pie_analyze` (the server's `@google/genai` client —
  already bundled, no local binary, **no ffmpeg anywhere**: the MP4 is produced in-engine
  by Media Foundation) and the `/visual-critique` skill. Without a key these return
  `feature_disabled` / are unavailable; everything else is unaffected.
- **Platform note:** video *recording* is Windows-only (Media Foundation); *analysis* of
  an existing MP4 is cross-platform (HTTPS call).
- **Detect:** `scripts/google-key.sh check`, or `GEMINI_API_KEY` /
  `GOOGLE_STUDIO_API_KEY` set in the environment.
- **Setup:** Part 4 of the skill (key from <https://aistudio.google.com/apikey> →
  `scripts/google-key.sh set`). One key covers both features.

### OpenAI API key + Python art stack — AI art (`/icon`, `/key-indicator-helper`)
- **Enables:** AI icon/keycap generation (`gpt-image-1`, including background removal —
  no local rembg/ImageMagick).
- **Needs:** the key (Part 3 of the skill) **and** Python 3.10+ with
  `pip install -r plugin/unreal-skills/skills/icon/requirements.txt` (openai, httpx, python-dotenv,
  Pillow; add numpy for the `/see` geometry steps).
- **Detect:** `scripts/openai-key.sh check`; `python -c "import openai, PIL"`.

### Python 3.10+ with Pillow + numpy — image geometry (`/see`, `/texture`, `/gimp-import` math)
- **Enables:** local image measurement/authoring scripts. No API key.
- **Detect:** `python -c "import PIL, numpy"` (try `python3` on POSIX).
- **Install:** official <https://python.org> · `winget install Python.Python.3.12` ·
  `brew install python` · distro package — or let **uv** provision it
  (`uv python install`, `uv pip install pillow numpy`). Then
  `pip install pillow numpy`.

### GIMP 3.x + the gimp-mcp server — layered UI-art pipeline (`/gimp-import`)
- **Enables:** authoring HUD/UI art as GIMP layers and importing them into UMG with
  layout preserved.
- **Needs:**
  1. **GIMP 3.2+** — official <https://www.gimp.org/downloads/> · `winget search gimp`
     (pick the 3.x id) · `brew install --cask gimp` · Linux: Flathub `org.gimp.GIMP`
     (distro `apt` may still ship 2.10 — the pipeline needs 3.2+). **User action**
     (GUI installer).
  2. **[maorcc/gimp-mcp](https://github.com/maorcc/gimp-mcp)** — the GIMP MCP server
     the `/gimp-import` skill is built against (GPLv3; Python, runs via `uv`, so
     Tier 2's uv covers it). This repo does **not** bundle or configure it
     (`.mcp.json` declares only `unrealMCP`); it has **three install steps**, all
     from its README:
     a. `git clone https://github.com/maorcc/gimp-mcp && cd gimp-mcp && uv sync`
     b. Copy its `gimp-mcp-plugin.py` into GIMP's per-user plug-ins dir
        (**Edit → Preferences → Folders → Plug-ins** shows the path; Windows:
        `%APPDATA%\GIMP\<major.minor>\plug-ins\gimp-mcp-plugin\`; the folder moves
        on minor GIMP upgrades) and restart GIMP.
     c. Register it in the MCP client under the name **`gimp-mcp`** (the skill's
        tool names depend on it):
        `claude mcp add gimp-mcp -- uv run --directory /path/to/gimp-mcp gimp_mcp_server.py`
     Runtime note: GIMP must be open with the in-GIMP listener started
     (**Tools → MCP → Start MCP Server**, TCP `localhost:9877`) before the tools work.
  3. Python Pillow + numpy (above) for `scripts/gimp_place.py`.
- **Optional extra:** ImageMagick — used only for one manual zoom/verify step in the
  skill; not required.
- **Detect:** GIMP on PATH (`gimp --version`) or standard install locations; a
  `gimp-mcp` entry in the user's MCP client config (`claude mcp list`).

### Docker + Neo4j 5 — Blueprint/graph projection (`/neo4j`, `plugin/unreal-skills/skills/neo4j/tool/`)
- **Enables:** projecting Blueprints/StateTrees/materials into a queryable Neo4j graph.
- **Needs:** any reachable **Neo4j 5** (Bolt). Docker is the documented default:
  the exact `docker run … neo4j:5` line lives in `plugin/unreal-skills/skills/neo4j/tool/README.md`. Neo4j Desktop
  or an existing server works too. The Bolt driver is fetched by `bun install`
  (automatic on first `scripts/neo4j.*` use).
- **Env:** `NEO4J_URI` (default `bolt://localhost:7687`), `NEO4J_USER` (default `neo4j`),
  `NEO4J_PASS` (**required**), optional `MCP_URL`, `UE_NEO4J_DATA_DIR`.
- **Detect:** `docker --version` + `docker info` (daemon up), or an already-reachable
  Bolt endpoint.
- **Install:** Docker Desktop (<https://www.docker.com/products/docker-desktop>,
  **user action** on Windows/macOS) · Linux: distro Docker Engine.

### Pulumi + AWS CLI — hosting / deploy toolchain
- **Enables:** standing up a game's service layer (dedicated servers, GameLift,
  matchmaking, sites) the way the sample game does. The **harness itself never shells
  out to these** — they're used per-project when you build a service layer, and
  `/gamelift` / `/networking` are advisory. Offer only under Full/Custom, and say
  an AWS account is a prerequisite.
- **Detect:** `pulumi version`; `aws --version` (+ `aws sts get-caller-identity` for
  configured credentials).
- **Install:** Pulumi — official `curl -fsSL https://get.pulumi.com | sh` /
  `winget install Pulumi.Pulumi` / `brew install pulumi`. AWS CLI v2 — official
  <https://aws.amazon.com/cli/> installer / `winget install Amazon.AWSCLI` /
  `brew install awscli`; then `aws configure` (**user action**: credentials).

---

## Explicitly NOT needed (don't invent these)

- **ffmpeg** — video is recorded in-engine (Media Foundation) and analyzed via the
  Gemini API; nothing in the repo shells to ffmpeg.
- **gh, jq, curl** — the core loop uses none of them (the server uses Bun's `fetch`).
- **ImageMagick** — explicitly avoided by `iconify.py` and `gimp_place.py`; only an
  optional manual zoom step in `/gimp-import` mentions it.
- **Node.js / npm / Python for the server** — the server is pure Bun; the old Python
  server is gone.
