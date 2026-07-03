# Architecture

The guiding star for this harness — what it is, how to conceive of it, and the
judgments that shape it. Read this before proposing a structural change; check the
proposal against the principles (§3), the things we hold (§4), and the things we
refuse (§5).

This is a conceptual document, deliberately free of line-level detail: everything
here should still be true after the next refactor that doesn't change behavior.
For operational contracts and per-tool foot-guns, see [`USAGE.md`](USAGE.md); for
the testing doctrine, [`TESTING.md`](TESTING.md); for setup troubleshooting,
[`DEBUGGING.md`](DEBUGGING.md).

---

## 1. Intent — what this is and why

**The primary consumer of this system is an LLM agent, not a human.** That single
fact drives every design decision. Agents are strong at composing many small,
well-specified operations and reasoning over structured results; they are weak at
recovering from surprising side effects, correlating unstructured logs, and
noticing that a "successful" call silently did the wrong thing. The harness is
shaped to amplify the strength and remove the failure modes: a large surface of
**granular, typed, deterministic primitives** over a live Unreal Editor, with
uniform errors, previewable mutations, and one observable timeline.

The second load-bearing decision is the **harness/game split**. This repository is
a reusable workbench — server, editor plugin, skills, tests, tooling — and the
game being built lives in `projects/<Name>/`, loading the plugin *by reference*
and carrying its own source control. The batteries-included development
environment never pollutes the game it builds, and one harness serves every game.

Self-hosted and vendor-independent by construction: everything runs from a clone
on the developer's machine; the whole stack is inspectable and grep-able.

## 2. The conceptual model — how it works

Two cooperating processes bridge the MCP world and the Unreal world:

| Component | Runtime | Role |
|---|---|---|
| MCP server (`src/server/`) | Bun / TypeScript | Speaks streamable-HTTP MCP to the client; validates every call against a Zod schema; forwards it as a JSON command over a local TCP socket |
| Editor plugin (`src/Plugin/UnrealMCP/`) | C++, UE Editor module | Listens on the TCP socket; routes each command to a per-domain handler on the game thread; wraps the result in the uniform envelope |

A tool call flows: client → schema validation (server) → TCP → **gates** → domain
handler → envelope back. The gates run in a fixed order on the plugin side:

1. **Boot gate** — nothing but status probes dispatches until the editor is fully
   initialized; the server pends calls during boot instead of failing them.
2. **PIE gate** — asset mutators are refused while a Play-In-Editor session runs.
3. **Dry-run gate** — a mutation called with `dry_run: true` returns a diff
   instead of applying; mutators that can't preview yet refuse rather than
   silently applying.

The wire contracts that make the whole surface predictable:

- **One canonical name per operation.** The MCP tool name, `{domain}_{verb}`
  form, is the canonical agent-facing name, and wire name == C++ handler key.
  Tool name and wire name coincide for all but a small, enumerated,
  test-enforced translation set (per-tool `command:` overrides — the
  `statetree_* → st_*` family and `bp_add_node → add_blueprint_node` — plus a
  few legacy parameter aliases), kept in parity by `gate-error-parity.test.ts`
  and `aliases.test.ts`. New operations get one identical name in all three
  positions (USAGE §3).
- **One envelope.** Every response is `{status, result, error, error_code,
  error_hint}`. Error codes come from a **closed taxonomy defined once in C++ and
  mirrored in TypeScript** — both sides must change together.
- **One mutation contract.** Every asset mutator runs
  `PreEditChange → mutate → PostEditChange → MarkPackageDirty` and the asset is
  saved automatically; callers never issue their own saves.
- **One log.** All editor, PIE, live-coding, and MCP events interleave into a
  single sequenced stream (`MCP_Unified.log`) so an agent correlates cause and
  effect on one timeline.

Around that core sit the supporting layers:

- **Progressive disclosure** (server): ~260 tool schemas are expensive to load up
  front, so the surface can be served whole, as a searchable catalog, or as a
  code-execution sandbox that calls tools programmatically — three modes over the
  same registry.
- **Result compaction** (server): oversized results can return a digest plus a
  pageable handle instead of flooding the model's context.
- **Multi-agent coordination**: one editor is shared state. PIE access is
  serialized through a FIFO **lease** (one holder, queued waiters, TTL reclaim);
  full rebuilds serialize through a **build lock** exposed as REST endpoints on
  the server. Both exist so concurrent agents never stomp each other's sessions.
- **Skills** (`.claude/skills/`): agent-facing doctrine — setup, authoring,
  testing, advisory knowledge — ships with the harness so game projects don't
  have to carry it.
- **Two test harnesses**: an in-process bun suite for server logic, and a pytest
  suite that boots a **real editor** and drives every bridge operation end-to-end
  — the parity oracle that fails if an operation lacks coverage.

## 3. Design principles

1. **Granular typed primitives are the foundation.** Every operation an agent
   might want exists as its own typed tool. Composites are *layered atop*
   primitives — implemented in the server layer where their source is auditable —
   never instead of them, and never as opaque C++ interpreters.
2. **Transparent over magical.** No tool does something the agent can't predict
   from its name, signature, and description.
3. **Predictable over DWIM.** Same inputs → same effects. No inference of
   unstated intent.
4. **The console/Python escape hatch is a trusted-operator power tool, not a
   crutch.** The code it runs is authored by the operator driving the session,
   so arbitrary execution isn't a threat we design against — but every routine
   operation should still get a typed tool; that pressure keeps the typed surface
   growing.
5. **Inspection beats mutation, in priority and depth.** Agents read more than
   they write. Multi-resolution reads (brief → filtered → deep), reference
   graphs, and live reflection outrank another mutation tool.
6. **Dry-run, not undo.** Preview-before-apply prevents wrong state from being
   committed; undo assumes the agent can *recognize* wrong state after the fact,
   which is exactly the failure mode to design out.
7. **Structured errors everywhere.** A closed error-code taxonomy plus a
   human-readable hint on every error site. Agents recover from structured
   errors; they get stuck on unstructured ones.
8. **Single sequenced observability stream.** One timeline; no cross-log
   correlation puzzles.
9. **Open architecture, no lock-in.** Runnable from a clone, forever.

## 4. What we deliberately hold — preserve these

Deep investments that define the harness. Don't trade them away for surface area.

- **StateTree depth** — a full authoring/reading surface for UE 5.4+ StateTree,
  the modern successor to Behavior Trees.
- **EQS depth** — dedicated authoring tools rather than a lump under "AI".
- **Live AI runtime introspection** — inspecting a *running* agent's state,
  awareness, and perception during PIE is a major debugging win.
- **Full asset CRUD** with redirector fixup — not just read-only search.
- **The standardized C++ mutation contract** (§2). New handlers must follow it;
  the auto-save invariant depends on all of them following it.
- **The uniform envelope and closed error taxonomy**, applied at every error
  site and lint-enforced.
- **The single unified log.** Don't fragment it.
- **Live Coding hot-patch** — sub-second C++ iteration against the live editor
  for function-body changes.
- **Progressive disclosure and result compaction** — the token economics of a
  260-tool surface are part of the architecture, not an afterthought.
- **Openness** — vendor-independent, hackable, transparent.

## 5. What we refuse — deliberate non-goals

Each entry is a *decision*, not an oversight. Revisit only with explicit
rationale, and update this section in the same change.

- **Executable recipe/skill libraries served by the MCP.** Recipes-as-code rot
  fast and create "official way" pressure that conflicts with the agent's actual
  strength — synthesis from inspection. The *information* belongs in docs and
  consumer-side skills, not server-shipped code.
- **Undo/redo.** UE's transaction stack is per-editor-session and unreliable for
  programmatic edits. **Dry-run replaces undo** (§3.6): preview prevents wrong
  state instead of betting on detecting it afterward.
- **Black-box composite authoring.** Any composite is transparent server-side
  source calling primitives — never a C++-side spec interpreter the agent can't
  audit.
- **Server-shipped LLM guidance / cross-tool relationship maps.** Guidance is
  consumer-coupled (different clients, models, prompts); server-side copies
  drift from real tool behavior. It lives in docs and skills.
- **Procedural world-building helpers** (`construct_castle` and kin). World
  content is composition — build it from `actor_spawn` and the other primitives.
- **Behavior Tree + Blackboard authoring.** Superseded by the `statetree_*`
  surface (property bindings are the Blackboard equivalent). AIController
  classes still route through generic Blueprint creation.
- **`save_all_dirty`.** Every mutator already saves (§2); a bulk flush would only
  exist to defend against a non-auto-saving mutator, which we don't have.
- **Cosmetic graph editing** (node positions). Agents reason on topology, not
  screen coordinates.
- **A frozen engine-API symbol database.** Live reflection per query is always
  current; a frozen monolith drifts every UE release.
- **Vendor lock-in of any kind.** Hosted-only anything is a non-starter.

## 6. When proposing a change

Check any new tool, split, or batch against these questions:

1. **Does the primitive form exist?** If not, add it first — composites layer
   atop primitives (§3.1).
2. **Is it deterministic?** Same inputs → same effect (§3.3).
3. **Does it handle `dry_run`?** Implement preview, or register the tool as
   blocked so the bridge refuses rather than silently applying.
4. **Does it return the standard envelope,** with an `error_code` from the closed
   taxonomy — added on *both* sides if new?
5. **Does it follow the mutation contract** if it touches an asset?
6. **Is the name canonical** (`{domain}_{verb}`, lint-clean)?
7. **Is the description enough for an agent to use it correctly without reading
   the source?** The description *is* the product surface.
8. **Does it re-introduce something §5 refuses?** Then either it loses, or this
   document changes in the same commit with the reasoning.

**If a change conflicts with this document, the change loses or the document is
updated as part of the same change.** The guiding star is allowed to move — but
only deliberately.

## 7. Naming conventions

- **lower_snake_case, ASCII**, domain-first: `{domain}_{verb}(_{modifier})*` —
  `actor_set_transform`, not `set_actor_transform`.
- **Full-word domains** (`statetree_*`, never an abbreviation), one family per
  concept.
- **Inspection verbs**: `_brief`, `_query`, `_inspect`, `_list`, `_get`.
  **Mutation verbs**: `_set`, `_add`, `_remove`, `_create`, `_delete`.
- **No version suffixes.** A contract evolves through its parameters, not its
  name.
- Enforced by `bun run lint:names`.
