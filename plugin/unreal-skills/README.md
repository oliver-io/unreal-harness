# unreal-skills — portable Unreal Engine agent skills

A Claude Code **plugin** bundling the portable skills from the
[unreal-harness](../../README.md): everything an agent needs to verify, author,
analyze, and reason about an Unreal project, loadable into **any** repo — not just the
harness itself.

## Install (from any repo)

```
/plugin marketplace add oliver-io/unreal-harness   # or a local path to this repo
/plugin install unreal-skills@unreal-harness
```

Installing also auto-wires the session to the harness's MCP server (the plugin
manifest declares `unrealMCP` at `http://127.0.0.1:8765/mcp`) — so any repo gets the
full Unreal tool surface, **provided the harness server is running on this machine**
(`scripts/run-server` in the harness clone).

## What's inside (18 skills)

| Category | Skills |
|---|---|
| Verification | `capture-pose`, `visual-critique`, `video-critique`, `see` |
| Authoring | `icon`, `texture`, `gimp-import`, `key-indicator-helper`, `progress-video` |
| Design & docs | `architect`, `docs` |
| Advisors | `ue-expert`, `position`, `npc_logic`, `networking`, `gamelift` |
| Analysis | `neo4j` — includes the full Bun/TS Unreal→Neo4j ingester at `skills/neo4j/tool/` |
| Build | `build` |

## Prerequisite: the UnrealMCP tool surface

The advisor skills (`ue-expert`, `position`, `npc_logic`, `networking`, `gamelift`) are
self-contained knowledge bases. Most of the rest drive a live Unreal editor through the
harness's MCP server (`pie_capture_from_pose`, `pie_record_*`, `video_analyze`,
`asset_list`, …) — so the harness's server + editor plugin must be running and connected
to your session. The harness is installed once per machine; this plugin makes its
*skills* portable, not the engine bridge itself. See the harness README / `/onboard`.

Skills that operate the harness repo itself (`onboard`, `bootstrap`, `automated-tester`,
`refactor`) intentionally stay in the harness's `.claude/skills/` and are not part of
this plugin.
