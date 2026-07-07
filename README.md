# Unreal Claude Harness

A multitool harness for Unreal Engine and Claude Code.  Not Another MCP.

# Not Another MCP

This project lets Claude Code use:

1) [**Unreal Editor Actions**](./docs/USAGE.md) [^1] for almost everything in the UI
2) **Multi-model Multimodality** [^2] to see & test visuals
3) **AWS, TypeScript & Pulumi** [^3] for hosted, multiplayer games
4) **Neo4j for Blueprints & Graphs** [^4]
5) **GIMP** [^5] image editor
6) **Open-Source Sample Game** [^6]
7) *Parallelism* and some locking primitives for simultaneous agents
8) **Custom Skills** - Has skills, each hard-fought and curated with guidance, for:
 - `/onboard` & `/bootstrap` - Set up your machine for UE development & set up a new game
 - `/architect` - C++-forward Architecture & Taste
 - `/docs` - Document & Spec-driven Design
 - `/see` & `/capture-pose` - Visual Feedback and Capture from the Editor or PIE
 - `/automated-tester` - Automated Headless & UI Testing & Capture-driven Loops
 - `/position` - In-editor & in-game Spatial Reasoning
 - `/networking` - Multiplayer Networking Guidance/Debugging
 - `/gamelift` - Dedicated-server Hosting & Matchmaking Guidance
 - `/npc_logic` - State Tree and Perceptual Systems Guidance
 - `/refactor` - Claude-Code driven Refactoring Loops in multiple flavors
 - `/build` - Create builds and distributions 
 -  ... **& more**, and constantly growing.  In my actual game project that I use to inform MCP development, I have some 40 skills that I regularly use.
 9) **Screenshot and video capture**

 And ***Coming Soon***, from my other project:

 9) **SteamSDK** and **Epic Online Services** tooling and examples
 10) **Mesh Generation** && **Animation Generation**
 11) **Blender** support

Uses its own Typescript/C++ Unreal Engine MCP, not the official Epic Games MCP.  Compatible with UE 5.7.

## Action Shots: Editor & Sample Game
<table>
  <tr>
    <td width="50%"><video src="https://github.com/user-attachments/assets/7259fcfe-263e-405f-a77e-406d1d5c7b67" width="400" muted playsinline controls></video></td>
    <td width="50%"><video src="https://github.com/user-attachments/assets/07cee163-6330-4b84-abee-7f159f515ee6" width="400" muted playsinline controls></video></td>
  </tr>
</table>

## Install and Run

Assuming you already have [**Bun**](https://bun.com/get), [Claude Code](https://code.claude.com/docs/en/quickstart), and the [**Unreal Engine**](https://www.unrealengine.com/download):

```bash
git clone https://github.com/oliver-io/unreal-harness
cd unreal-harness
bun install
bun run mcp
claude # ask claude to /onboard - it surveys your machine, asks minimal vs full, and walks you through the automated or manual installs
```

That starts the MCP server at `http://127.0.0.1:8765/mcp`.  Open Unreal with the plugin enabled and the bridge comes alive on `127.0.0.1:55557`.

### Dependencies

**Minimal** — enough to drive a live editor with all ~285 tools — is just Bun, the Unreal Engine, git, and a C++ toolchain UE can build with (on Windows that's VS 2022 with the "Game development with C++" workload).  Everything else is optional and per-feature: `uv` for the test suite, a Gemini key for video analysis & visual critique, an OpenAI key + Python for AI art, GIMP for the layered-UI pipeline, Docker + Neo4j for graph projection, Pulumi + the AWS CLI for hosting.  `/onboard` detects what you have, asks what you want (minimal / full / pick-and-choose), and installs the gaps from official sources with your say-so — the full matrix lives in [the onboard skill's dependency doc](.claude/skills/onboard/DEPENDENCIES.md).


[^1]: Most basic editor actions are MCP-exposed.  Many are enriched with validation and guidance so that Claude does not repeat common mistakes (it struggles with spatial reasoning, for example).
[^2]: Optionally uses Gemini and OpenAI for image review and generation.  This allows the harness to create, see and analyze images. 
[^3]: For deployments for Multiplayer or Distribution.  Wired and guided to use TS/Pulumi/AWS to stand up a stack for the game, websites, queues, external services, hosting for dedicated servers, et cetera.  This means the barrier to playing your game can shrink down to about "making it worth playing".
[^4]: Dumps blueprints and UE graph structures to Neo4j, with enriched ontologies, to understand large Blueprint structures (like animation blueprints, state machines, state trees, etc) or third-party BP integrations.  Crucially, cross-links these to UE external references with edges in Neo4j, rather than a naive data dump.
[^5]: For 2D Textures and Assets.  Optionally capable and guided to use GIMP and understand layers to build complicated UI widgets, HUDs, etc. either manually (you can skip elaborate export chains and let Claude handle them) or otherwise.  GIMP is driven through [maorcc/gimp-mcp](https://github.com/maorcc/gimp-mcp), an external GIMP 3.2 MCP server — `/onboard` documents the setup.
[^6]: HOVERBALL, the sample game in this repository.


You may be starting to see, this isn't really an MCP.  This is a thick set of harness tools, some of which are really opinionated, but only because *I made them when I needed them.*  Like you should build the kind of games **you** want to play, so too should you build the kind of tools that you need to do it (if they don't exist already, and for me, they didn't).

# What Does It Do?

It's meant to be a tool that can transfer my software engineering skills into game development.  Make no mistake: I am not an Unreal Engine expert, and that's the point (you don't need to be one either).  If you are one, this is probably pretty cool for you too, especially if you know your way around Claude Code already.

Essentially it's a suite of tools that exposes guidance and Unreal Engine-level functionality.  There are deliberate, opinionated processes that it encodes, such as testing methodology and networking preferences.  It's done that way so that this repository can get a non-expert through the process of making great-looking, great feeling games -- and ship them into distribution & production -- without needing to spend a lot of time picking up the tooling layer.

# Is This For AI Slop?

I mean, if you want it to be.  But I'm not the barrier to entry here; you can already take Claude Code, and ask it to connect up to the exposed TCP server of UE, and have it make you some game that no one wants.  I won't stop you from doing that, and I won't stop you from using my tools to do it with a bit more convenience.

## Not Enough Time to Learn Everything

I am usually pretty busy with work, life, musical instruments, hobby projects in software, hobby projects around the house.  life be crazy.

I have lots of game ideas, most I never even start.  You (reader) may enjoy some of these if I made them, others probably not.  If I **never make the idea into a game**, you'll never find out.  I feel that many of you out there are just like me.  Maybe like me, you even tried and failed to pick up real expertise in UE (or another game engine), for good reasons, it's a dense tool.  I know code.  I treat this harness much like I would treat a buddy or coworker that knows UE, and that's all I need to be dangerous.  That's all I have to say on the subject of AI slop.  Go forth, make cool games, and be merry.

# Docs

Docs will follow.  For now, Claude is your best resource for understanding how to use it, as it's pretty decently documented in the harness itself.

# Sample Game

I've included a little/big sample game along with the repository.  This is useful because it allows your Claude Code session to look at the structure, code, and potentially even runtime of a fully-functional game.  This can let it escape certain pitfalls that are not obvious to it up-front, or even just give you a solid place to begin if your game's architecture is similar.

## Sample Game Assets

Currently working on adding stubs of the paid & licensed assets in HOVERBALL.  Because I paid for these (credits for the assets below), I can't include them in the open-sourced version of the game, which means you might get a "box" type experience loading some things, like the motorcycle, in the sample game.

When this work is done, you should then be able to actually open HOVERBALL in the UE engine from source checkout.  However, right now, I've only included the game's C++, TypeScript service-layer and documentation.

## Sample Game Structure

The sample game docs are still being split out.

## Sample Game Hosting / Multiplayer

Check it out.  It's live at [HOVERBALL.gg](https://hoverball.gg) and you can download it, queue up for a game, and play it yourself.  You can even open an issue or contribute, it isn't perfect, but it's a fully functioning game.

### In-Game Authentication & Signing

Authentication for games is tricky.  I might roll in an OAuth provider at some point, because doing auth over Steam or Epic Online Services requires legal documents, review periods, and approvals that prevent immediate distribution for things like demo concepts.  Of course, it's not really advisable to install random games.  I'm working on getting a certificate[^7] to sign the installers & et cetera, but until then, you may have to trust in the authenticity of Some Guy to play HOVERBALL.  If you do, feel free to ignore the **Unknown Publisher** alerts.  Promise not to ransomware you.

[^7]: Surprisingly annoying to sign a Windows binary.  These are the kinds of things I try to avoid learning, but *c'est la vie* in games.

# Does It Work Perfectly?

Nope!  This project is actively a WIP.  I can be enticed to work harder for stars, likes, views, beer, good wishes, positive vibes, and detractors.  Yes, that's right, detractors too.  i grow stronger with each internet detractor.  my controversy shall bloom like a very pretty flower.  feed me your rage, or just constructive criticism, that also helps.

# Caveats, Warnings, and Unfinished Work
- I haven't done a thorough security review on some of the AWS stuff, as it's mostly for testing your game.  If you want to advance to a legit prod environment, you'll need to do some more engineering.  I have a Tactical Medieval MMORPG (*lmfao*) I am working on, which has a thorough stack, and I'll gradually port the patterns into this repo.
- Tons of things are messed up by Claude on the regular, so don't get the impression you can one-shot GTA6.  You will find it is still hard to make complex games, because they are complicated.
- I ported this entire harness out of a thicker, more opinionated game project.  There is probably some slop and poor instructions, but I'm going to switch over to eating this repo as my own dogfood and I'll fix things.

# Tests
A lot of this stuff is difficult to test but I do use some automated tests to keep the agentic development on track.  Actively improving this.

# License

MIT — see [LICENSE](LICENSE).

# Asset Credits
[meshal.97](https://www.fab.com/sellers/meshal.97) - motorcycles
[SpectraMotions](https://www.fab.com/sellers/SpectraMotions) - motorcycle anims
[Harhtif](https://www.fab.com/sellers/Harhtif) - drone
[VFX Vault](https://www.fab.com/sellers/VFX%20Vault) - explosion VFX

# Troubleshooting, Contributions, and Gripes

- open an issue or a PR and I will fix it if it is broken
- @oliver-io on twitter to complain or cheer
- catch me around town

good luck & have fun playing the Unreal Engine editor
