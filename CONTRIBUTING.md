# Contributing

Thanks for your interest in this project. It's a **Claude-first harness** for driving a
live Unreal Editor over MCP — an MCP server (Bun/TypeScript), a UE editor plugin (C++),
authoring skills, and a parity test suite. Please read [`CLAUDE.md`](CLAUDE.md) and
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) first; they are the source of truth for how
the pieces fit and what the project deliberately holds or refuses.

## Ground rules

- **Read the architecture before proposing structure.** `docs/ARCHITECTURE.md` §4/§5 lists
  what is intentionally held back and what is refused by design (recipe/skill libraries,
  undo/redo, world-building composites, …). A convenience composite may be a "no" on purpose.
- **Granular typed primitives first.** Composites belong in the server layer
  (grep-able/auditable), never in C++.
- **Keep the response envelope uniform and the error taxonomy closed.** Every tool returns
  `{status, result, error, error_code, error_hint}`; every error uses an `EMCPErrorCode`.
- **Same inputs → same effects.** Mutators should support `dry_run` where it's meaningful.

## Development setup

Run [`/onboard`](.claude/skills/onboard/SKILL.md) (locates your engine, wires the plugin into
a host project) or set `UNREAL_ENGINE_ROOT` / `UNREAL_PROJECT_ROOT` by hand — see
[`.env.example`](.env.example). The MCP server lives in `src/server/`; the plugin in
`src/Plugin/UnrealMCP/`.

## Before you open a PR

Run the editor-free gates from `src/server/` (these are what CI runs):

```bash
cd src/server
bun install
bun run typecheck      # tsc --noEmit
bun run lint:names     # canonical tool-naming policy (exit 1 = violation)
bun test test/*.test.ts # unit + in-process MCP protocol/integration
```

If you touched anything that drives a live editor, also run the pytest parity oracle
(`tests/run.ps1` / `tests/run.sh`) locally — it builds and launches a real editor and is not
part of CI. See [`docs/TESTING.md`](docs/TESTING.md) for the testing doctrine, or run the
`/automated-tester` skill.

## Conventions

- **Server tool:** add a `ToolDef` in the relevant `src/server/src/domains/<domain>.ts` via
  `bridgeTool({...})` or `defineTool({...})`; the Zod `description` *is* the LLM's guidance —
  keep it accurate. Name canonically: `{domain}_{verb}(_{modifier})*`, `lower_snake_case`.
- **C++ command:** follow the author's contract in `docs/USAGE.md` §3
  (`PreEditChange → mutate → PostEditChange → MarkPackageDirty`; structured error responses).
  A new gate or error code must be mirrored on **both** sides — the
  `src/server/test/gate-error-parity.test.ts` check enforces this.
- Keep `.cpp`/`.h` files ≤ ~600 LOC. Update `CHANGELOG.md` (server) or
  `src/Plugin/UnrealMCP/CHANGELOG.md` (plugin) in the same change.

## Reporting bugs / security

File an issue for bugs and capability gaps. For anything security-relevant, read
[`SECURITY.md`](SECURITY.md) — the trust boundary is your local machine — and use a private
disclosure channel.

By contributing, you agree your contributions are licensed under the repository's
[LICENSE](LICENSE) and that you follow the [Code of Conduct](CODE_OF_CONDUCT.md).
