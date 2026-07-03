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

**`something is already listening on 127.0.0.1:8765`** — the run script refuses to
double-bind by design. The usual culprit is a stale/orphaned server from a previous
session (rarely an unrelated app). `scripts/stop-server.sh` / `scripts/stop-server.ps1`
kills the listener; then re-run `scripts/run-server.{sh,ps1}`.

**First start is slow** — dependency resolution happens on the first `bun install`.
Subsequent starts are fast.

**Sanity check the environment** (from the repo root):

```bash
bun run typecheck
```

This compiles the server without starting it (`bun run mcp` takes no flags — any
invocation boots a real server on :8765). Once the server *is* running, the real
probe is the §3 smoke sequence: `ping` → `mcp_status`.

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

**Commands land on the wrong editor / asset saves hit file locks** — a **second
editor instance** is running. Only one process can bind :55557; the duplicate's
bind fails *silently* (just an error line in its own log — grep it for
`Failed to bind listener socket`), so it runs bridge-less while the first editor
keeps the bridge, and it can hold `.uasset` file locks that break saves. Fix:
close the instance that does **not** own port 55557. Details:
[`BUGS.md`](BUGS.md) → "Duplicate editor on :55557 fails its bind silently".

## 4. Environment-coupled tools return a structured error

A few tools depend on host-side environment; everything else editor-connected
works without these.

| Variable | Consumed by | Error when unset |
|---|---|---|
| `UNREAL_PROJECT_ROOT` | `editor_read_logs` (locates `Saved/Logs/MCP_Unified.log`), `editor_build_game_target` | `invalid_argument` naming the variable |
| `UNREAL_ENGINE_ROOT` | `editor_build_game_target` (locates `Engine/Build/BatchFiles/Build.bat`) | `invalid_argument` naming the variable |
| `GEMINI_API_KEY` (fallback: `GOOGLE_STUDIO_API_KEY`) | `video_analyze` / `pie_analyze` — a pure server-side Gemini call; the editor never sees the key | `"No Gemini API key configured. Set GEMINI_API_KEY (or GOOGLE_STUDIO_API_KEY) in the repo-root .env and restart the server."` |

Export them before `run-server`, or in a wrapper script. `editor_build_game_target`
discovers the `.uproject` by glob inside the project root and derives the UBT
target name from its filename (override with the `target` parameter).

For the video key, `scripts/google-key.sh` (`check` / `set`, key read from stdin)
stores `GOOGLE_STUDIO_API_KEY` in the gitignored repo-root `.env`, which Bun
auto-loads — restart the server afterwards. (`scripts/openai-key.sh` is the same
pattern for `OPENAI_API_KEY`, but that key feeds host-side art-generation skills
only — the server never uses it.) Video-analysis knobs — provider, model,
analysis fps, upload timeout — are the `UNREAL_MCP_VIDEO_*` env vars in
`src/server/src/config.ts`.

## 5. The editor logs say more than the response

Every `status: success` envelope means the command was *dispatched and handled* —
for side-effect-heavy paths, confirm via the matching read tool or the unified
log rather than the response alone. `editor_read_logs` (requires `UNREAL_PROJECT_ROOT`)
filters the single sequenced stream `[LOG:…] [PIE:…] [LIVECODING:…] [MCP:Command]`
server-side; `grep`/`category`/`since_seq` keep the output small.

## 6. Build scripts refuse with exit 75

C++ builds are serialized through the server's build lock (plain-HTTP `/build`
endpoints on the same :8765 listener). While another agent's build holds the
lock, the build scripts (e.g. `scripts/build-editor.ps1`) refuse **before**
invoking `Build.bat` — a *"can't build right now"* message and **exit code 75**.
This is not a failure: wait and poll `GET http://127.0.0.1:8765/build/status`
until `in_progress` is `false`, then re-run. **Never kill the other build.** A
crashed holder frees the lock automatically (PID liveness); a hung-but-alive one
is bounded by the lock TTL. Contract: [`USAGE.md`](USAGE.md) §2.17 / §3.6.
