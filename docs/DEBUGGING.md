# Debugging & Troubleshooting Guide

Setup-time troubleshooting for the two-process chain:

```
MCP client (Claude Code / Cursor / Windsurf)
    │  streamable-http → 127.0.0.1:8765/mcp
    ▼
MCP server (src/server/ — Bun/TypeScript)
    │  TCP/JSON → 127.0.0.1:55557
    ▼
Unreal Editor with the UnrealMCP plugin enabled
```

Each hop fails differently. Work top-down: server up? client connected? editor reachable?

For operational (post-setup) guidance — tool semantics, compile/save discipline,
per-domain foot-guns — see [`USAGE.md`](USAGE.md).

---

## 1. The server won't start

**`'bun' not found on PATH`** — the run scripts require [Bun](https://bun.sh/);
install it and re-run. There is no manual dependency step beyond `bun install`:
`bun run mcp` at the repo root resolves dependencies from the root `package.json`
+ `bun.lock` on first invocation. Do **not** hand-edit `node_modules` — that
bypasses the lockfile.

**`something is already listening on 127.0.0.1:8765`** — a previous server (or an
unrelated app) holds the port. `scripts/stop-server.sh` / `scripts/stop-server.ps1`
kills the listener; then re-run `scripts/run-server.{sh,ps1}`.

**First start is slow** — dependency resolution happens on the first `bun install`.
Subsequent starts are fast.

**Sanity check the environment** (from `src/server/`):

```bash
bun run mcp --help
```

## 2. The client doesn't see the tools

The server speaks **streamable-http**, not stdio. Wire it into the client as a URL
entry — do not configure a `command`/`args` stdio launch:

```json
{
  "mcpServers": {
    "unrealMCP": {
      "type": "http",
      "url": "http://127.0.0.1:8765/mcp"
    }
  }
}
```

(Claude Code: `.mcp.json` at the project root. Other clients: their MCP config
equivalent, using the same URL form.)

**Tools disappear mid-session** — client-side registry drift. Reconnect the MCP
server from the client; the MCP server process itself survives editor restarts and
client disconnects, so it almost never needs a bounce.

**Tools genuinely changed** (you edited a domain registry under
`src/server/src/domains/*.ts` or `register.ts`) — restart the server
(`stop-server` + `run-server`); the server serves the Zod tool catalog loaded at
startup.

## 3. Tools error: the editor isn't reachable

| Error | Meaning | Fix |
|---|---|---|
| `Failed to connect to Unreal Engine ... 127.0.0.1:55557` | Editor not running, or the UnrealMCP plugin isn't enabled in the project | Start the editor; check **Edit → Plugins → UnrealMCP** is enabled |
| `error_code: editor_not_ready` | The editor process is up but still initializing — the plugin's boot gate refuses dispatch until the editor is interactive | Normally invisible: the server **pends** calls up to ~120 s and proceeds automatically. Surfaces only on very long boots — poll `mcp_status` until `result.ready == true` |
| `error_code: pie_active` | A Play-In-Editor session is running; asset load/mutation commands are refused during PIE | Stop PIE (`pie_stop`) and retry. Read-only registry queries, PIE input/screenshots, and AI-runtime reads still work during PIE |
| Transient `connection forcibly closed` / reset on a single call | The editor was under heavy load (asset compiles, PIE churn) and dropped one TCP accept | Retry the call — the next attempt typically lands. If it persists, check the editor is responding at all |

Two probes cut through ambiguity, and both work while the editor is booting:

- `ping` — round-trips the whole chain with no editor work.
- `mcp_status` — returns `{ready, phase, pie_active}` from the plugin's network
  thread.

A good smoke sequence after first setup: `mcp_status` → `ping` → `asset_list`
(read) → any mutator with `dry_run=true`.

## 4. Project-coupled tools return a structured error

Two tools need to know where the host project and engine live; everything
editor-connected works without these.

| Variable | Consumed by | Error when unset |
|---|---|---|
| `UNREAL_PROJECT_ROOT` | `read_logs` (locates `Saved/Logs/MCP_Unified.log`), `build_game_target` | `invalid_argument` naming the variable |
| `UNREAL_ENGINE_ROOT` | `build_game_target` (locates `Engine/Build/BatchFiles/Build.bat`) | `invalid_argument` naming the variable |

Export them before `run-server`, or in a wrapper script. `build_game_target`
discovers the `.uproject` by glob inside the project root and derives the UBT
target name from its filename (override with the `target` parameter).

## 5. The editor logs say more than the response

Every `status: success` envelope means the command was *dispatched and handled* —
for side-effect-heavy paths, confirm via the matching read tool or the unified
log rather than the response alone. `read_logs` (requires `UNREAL_PROJECT_ROOT`)
filters the single sequenced stream `[LOG:…] [PIE:…] [LIVECODING:…] [MCP:Command]`
server-side; `grep`/`category`/`since_seq` keep the output small.
