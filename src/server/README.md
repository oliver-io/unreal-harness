# Unreal MCP Server (Bun)

Modern, Bun-native [Model Context Protocol](https://modelcontextprotocol.io)
server that drives a **live Unreal Editor**. Speaks streamable-HTTP MCP to the
client and forwards each tool call as JSON to the editor's C++ bridge over TCP.

## Quickstart

All commands run from the **repo root** — that's where `package.json` (and the
lockfile) live; this directory holds the source.

```bash
bun install
bun run mcp            # serve on http://127.0.0.1:8765/mcp
```

That's it — no venv, no Python, no build step. The editor bridge is discovered on
`127.0.0.1:55557`; tools return a clean "engine unavailable" error until the
editor is up, then work automatically (a boot gate holds commands until the
editor is interactive).

```bash
bun run mcp:stop       # free the port
bun test               # unit tests (pure logic; no editor needed)
bun run typecheck      # tsc --noEmit
bun run lint:names     # canonical tool-name lint
bun run build          # single-binary: dist/unreal-harness
```

### Configuration (env, all optional)

| Var | Default | Meaning |
|---|---|---|
| `UNREAL_MCP_HOST` / `UNREAL_MCP_PORT` | `127.0.0.1` / `8765` | MCP HTTP listener |
| `UNREAL_BRIDGE_HOST` / `UNREAL_BRIDGE_PORT` | `127.0.0.1` / `55557` | editor bridge |
| `UNREAL_MCP_LOG_LEVEL` | `info` | `debug`\|`info`\|`warn`\|`error` |
| `UNREAL_MCP_SURFACE` | `full` | `full`\|`compact`\|`code` (see below) |
| `UNREAL_MCP_MAX_RESULT_BYTES` | `0` (off) | compact results larger than N bytes to a handle |
| `UNREAL_MCP_VIDEO_PROVIDER` | `google` | video-analysis backend (`video_analyze` / `pie_analyze`) |
| `UNREAL_MCP_VIDEO_MODEL` | `gemini-3.5-flash` | Gemini video-understanding model id |
| `UNREAL_MCP_VIDEO_ANALYSIS_FPS` | `1` | default fps the model samples an upload at |
| `UNREAL_MCP_VIDEO_MAX_ANALYSIS_FPS` | `30` | cap on requested analysis fps |
| `UNREAL_MCP_VIDEO_UPLOAD_TIMEOUT_MS` | `120000` | upload wall-clock timeout |
| `GEMINI_API_KEY` | — | video-analysis key (`GOOGLE_STUDIO_API_KEY` accepted as fallback) |

## Token efficiency — progressive disclosure

A few hundred tool schemas (~285 domain tools, ≈290 registered — the boot log
prints the authoritative `[surface=…, N/M tools advertised]` figure) cost
~80–120k tokens if loaded up front. Three surface modes ("compaction of
operations until requested"):

| `UNREAL_MCP_SURFACE` | advertises | the full surface is reached by |
|---|---|---|
| `full` (default) | every tool + `catalog_*` | called directly (best with client-side tool-search) |
| `compact` | ~6: `mcp_status`, `catalog_*`, `result_read` | `catalog_search` → `catalog_describe` → `catalog_call` |
| `code` | ~8: above + `code_api`, `code_run` | writing TS that calls `unreal.<tool>(params)` |

- **`catalog_*`** (Tier 2): `catalog_domains` → `catalog_search` → `catalog_describe`
  → `catalog_call`. Schemas enter context only when a tool is actually inspected.
- **`code_run`** (Tier 3): execute a TS snippet that drives the editor; loop /
  filter / aggregate in code so only your `console.log` + return value reach the
  model — "list 5000 assets, keep 3" costs 3 rows, not 5000. Read the API first
  with `code_api`.
- **Result compaction** (`UNREAL_MCP_MAX_RESULT_BYTES`): off by default; when on,
  oversized payloads return a digest + handle you page via `result_read`.

> **⚠️ code-mode security.** `code_run` executes agent-authored TypeScript in a
> Bun Worker. The Worker isolates crashes and enforces a wall-clock timeout, but
> is **not** a security sandbox — the snippet has the same trust as any tool call
> (it can already invoke every tool). It is therefore **opt-in** (`code` surface
> mode only) and intended for local, single-user editor automation. Do not enable
> it on a shared/remote deployment without a real sandbox in front.

## Architecture

```
src/
  main.ts            entry — streamable-HTTP transport on Bun (node:http)
  server.ts          registry → MCP SDK tools
  register.ts        assembles the registry from every domain module
  config.ts          env + defaults
  log.ts             leveled stderr logger
  bridge/            connection.ts (TCP, boot gate, retry, framing)
                     envelope.ts · errors.ts · lifecycle.ts
                     gates.ts (PIE/dry-run sets, mirrors C++)
  registry/          index.ts (register/search/describe/dispatch) · types.ts · aliases.ts
  domains/           one module per domain = the tool surface (34 + core)
                     _shared.ts (bridgeTool/defineTool) · _schemas.ts (Vec3, …)
  build/             http.ts (REST /build endpoints) · lock.ts (build lock)
  pie/               lease.ts (PIE lease) · reconciler.ts
  video/             analyzer.ts (video_analyze backend, vendor-quarantined)
  disclosure/        metatools.ts (catalog_*) · codemode/ (generate, worker, sandbox, tools)
  compaction/        handles.ts (LRU digest) · tool.ts (result_read)
test/                bun test (unit; integration is editor-gated)
```

Tools are **data**: every operation is a `ToolDef` (Zod input = single source of
truth for schema + validation + types, plus annotations and a handler). The three
token-efficiency disclosure tiers and result compaction are layers over this same
registry.

## Invariants

- **Wire == tool name == C++ handler key** (post naming-migration). The envelope
  `{status, result, error, error_code, error_hint}` and the closed error-code
  taxonomy mirror the C++ plugin exactly; neither changes here.
- **No escape hatches, no convenience composites** — keep the typed surface
  complete instead (`../../docs/ARCHITECTURE.md`).
