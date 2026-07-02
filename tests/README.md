# UnrealMCP integration tests

A pytest harness that boots a **real Unreal Editor** (headless or GUI), drives
the `UnrealMCP` plugin's commands over its TCP bridge, and asserts on the
results. It exists to answer "do these MCP operations actually do the right
thing against a live engine?" without a human in the loop.

## How it works

Tests drive the **real MCP server** — the actual product surface — so they
exercise the Zod tool-registry layer (tool names, kwarg→bridge-param mapping,
multi-call orchestration, response shaping) and the MCP protocol, not just the
engine handlers:

```
pytest  ──>  EditorSession (harness/editor.py)      MCPServer (harness/server.py)
                 │  build + launch editor,               │  `bun run mcp` (repo root)
                 │  poll mcp_status until ready           ▼  Bun MCP on :8765/mcp
                 ▼                                  MCPClient (harness/mcp_client.py)
            UnrealEditor[-Cmd].exe                       │  streamable-HTTP, call_tool(name, kwargs)
            (UnrealMCP plugin on :55557) <───────────────┘  (the `mcp` fixture)
```

The `mcp` fixture calls tools **by name with their tool kwargs** — exactly what
an MCP client (Claude Code, etc.) does. `mcp.expect(tool, kwargs)` unwraps the
`{status, result}` envelope just like the old bridge client, so assertions read
the payload directly.

A handful of ops are **bridge-internal** — they have no standalone MCP tool
(`ping`, the legacy `spawn_actor`/`spawn_blueprint_actor`,
`get_blueprint_material_info`). Those few are dispatched through the low-level
`bridge` fixture (raw TCP/JSON to `:55557`, `harness/bridge_client.py`), with a
comment at each site. Everything else goes through the MCP.

> The tool layer genuinely remaps things — e.g. `actor_spawn` takes `class_path`
> (mapped to the bridge's `class`), `create_material` takes a single
> `material_path` (split internally), `statetree_state_add` takes `name` (not
> `state_name`), `tag_move` takes `from_tag`/`to_tag`, `add_blueprint_node` is
> the `add_node` tool with flattened kwargs. The authoritative tool→kwargs map is
> `harness/tool_schemas.json` (regenerate by listing tools off a running server).

### Two modes — headless vs GUI

| Mode | Binary + flags | Renders? | Use for |
|---|---|---|---|
| `headless` (default) | `UnrealEditor-Cmd.exe <proj> -nullrhi -nosound -unattended -nopause -nosplash` | No GPU/pixels | Everything except render-dependent ops |
| `gui` | `UnrealEditor.exe <proj>` (real RHI + window) | Yes | Screenshots, thumbnails, lightmaps |

`-nullrhi` still boots the **full** editor — `OnEditorInitialized` fires, so the
plugin's boot gate opens and every non-render command works. Tests marked
`@pytest.mark.render` are auto-skipped unless you pass `--ue-mode=gui`.

### Isolation / cleanup

- **Editor-per-session, not per-test.** Boot is expensive, so one editor serves
  the whole run. Tests are written as `mutate → inspect → assert`.
- **Actor/level tests need no cleanup** — they spawn into the unsaved transient
  level, which evaporates when the editor quits without saving.
- **Tests that save assets** put them under `/Game/__MCPTest__/<test>` (via the
  `mcp_namespace` fixture), which is deleted in-editor at test end and wiped on
  disk (`Content/__MCPTest__/`, gitignored) at session end. This is the
  lightweight alternative to "git revert the project": nothing test-created is
  tracked, so there is nothing to revert.

## Prerequisites

1. Unreal Engine installed (5.4+; the plugin targets 5.5/5.7). Set:
   ```
   UNREAL_ENGINE_ROOT = <dir containing Engine/>     # e.g. C:\Program Files\Epic Games\UE_5.5
   ```
2. A C++ toolchain UE can build with (Visual Studio on Windows). First run
   compiles the editor target + the UnrealMCP plugin (minutes); later runs are
   incremental.
3. `pip install -r tests/requirements-test.txt` (just pytest).

The host project lives in `tests/fixtures/TestProject` — a minimal C++ project
whose only job is to be a compilable host for the plugin. The harness junctions
`src/Plugin/UnrealMCP` into its `Plugins/` automatically; `Binaries/`,
`Intermediate/`, `Saved/` are gitignored.

## Running

Zero-setup wrappers use `uv` to supply pytest in an ephemeral env — nothing to
install:

```bash
tests/run.sh                  # bash / Git Bash
tests\run.ps1                 # PowerShell
tests/run.sh --ue-mode=gui    # all args pass through to pytest
```

Or invoke pytest directly if you have it installed:

```bash
cd tests

# Headless (default): build if needed, boot -nullrhi editor, run non-render tests
pytest

# Force a clean rebuild of the editor target first
pytest --ue-build=always

# GUI mode — real window; also runs the @render screenshot test
pytest --ue-mode=gui

# Fast local iteration: attach to an editor you already have open
# (skips build + launch; just verifies mcp_status.ready)
pytest --ue-attach
```

### Options

| Option | Default | Meaning |
|---|---|---|
| `--ue-mode` | `headless` | `headless` (-nullrhi) or `gui` (real render) |
| `--ue-build` | `auto` | `auto` (build iff not built) / `always` / `never` |
| `--ue-attach` | off | don't launch/build; use an already-running editor |

### Environment variables

| Var | Default | Meaning |
|---|---|---|
| `UNREAL_ENGINE_ROOT` | — (required) | engine install root |
| `UE_MCP_TEST_PROJECT` | `tests/fixtures/TestProject` | override host project |
| `UE_MCP_BRIDGE_HOST` / `UE_MCP_BRIDGE_PORT` | `127.0.0.1` / `55557` | bridge endpoint |
| `UE_MCP_BOOT_TIMEOUT` | `420` | seconds to wait for the editor to go interactive |

## Operation coverage

There is one integration test (at least) for **every** bridge operation the MCP
exposes. This is enforced, not aspirational:

- `harness/operations.py` is the generated manifest of all bridge commands
  (regenerate with `python tools/regen_operations.py` after adding tools).
- Each test declares the ops it exercises with `@covers("op_name", ...)`.
- `integration/test_zz_coverage.py` statically scans for `@covers(...)` and
  **fails listing any operation without a test**. It's the scoreboard.

Every test follows the same shape: **arrange** prerequisite state (create the
parent asset, spawn the actor, …) → **dispatch** the op → **assert the resulting
state** via a read/inspect op or the on-disk `.uasset` (`config.uasset_disk_path`).

Markers / skips you'll see:
- `@pytest.mark.render` — needs real pixels (screenshots). Runs only in `--ue-mode=gui`.
- `@pytest.mark.gui_only` — opens an editor/asset window (widget editor, PIE,
  `open_asset`). Under `-nullrhi` the periodic Slate layout-save ticker fatals on
  the headless window, so these run only in `--ue-mode=gui`.
- Content-gated `pytest.skip` — ops needing assets the stock project lacks
  (a writable AnimSequence, an IKRigDefinition, a PhysicsAsset). They discover the
  asset and skip with a clear reason when absent; they RUN if the content exists.
  (The skeletal-mesh/skeleton/anim ops that only need a *skeleton* DO run, using
  the engine's `/Engine/EngineMeshes/SkeletalCube`.)

A skipped/gui_only test still counts toward coverage — the guard scans `@covers`
statically, so the manifest is satisfied regardless of run mode.

## Layout

```
tests/
  conftest.py              fixtures, CLI options, render/gui_only skip, cleanup
  pytest.ini · requirements-test.txt · run.sh · run.ps1
  tools/regen_operations.py   regenerate the op manifest from src/server
  harness/
    config.py              engine/project/path + env resolution, uasset_disk_path
    bridge_client.py       TCP/JSON client (mirrors the server wire contract)
    editor.py              build / launch / await-ready / auto-restart / teardown
    operations.py          generated manifest of all 235 bridge operations
    coverage.py            @covers decorator + static coverage scanner
    ops.py                 ensure_absent / assert_ready / first_asset_of / payload
  fixtures/
    TestProject/           minimal C++ host project (plugin junctioned in at setup)
  integration/
    test_smoke.py test_actor.py test_actor_level.py test_asset.py
    test_blueprint.py test_blueprint_graph.py test_material.py test_widget.py
    test_statetree.py test_statemachine.py test_niagara.py test_eqs.py
    test_data.py test_tags.py test_gas.py test_reflection.py test_reads.py
    test_mesh.py test_animation.py test_skeleton.py test_physics.py
    test_ik.py test_kinematics.py test_pie.py test_screenshot.py
    test_zz_coverage.py    the coverage guard (runs last)
```

Current state (headless): **169 passed, 32 skipped, 0 failed** — the 32 skips are
the content-gated and gui_only ops above. Run `--ue-mode=gui` to exercise the
gui_only/render set.

## Adding a test

Most tests follow the same shape, through the `mcp` fixture:

```python
def test_my_op(mcp):
    result = mcp.expect("some_tool", {"kwarg": "value"})   # raises on error result
    check = mcp.expect("some_inspect_tool", {...})
    assert check[...] == expected
```

`mcp.expect(tool, kwargs)` returns the unwrapped `result` and raises on a
non-success result. Use `mcp.command(...)` when asserting on an *expected* error
(e.g. error_code taxonomy). Verify tool kwargs against `harness/tool_schemas.json`
(the tool layer often renames them vs the bridge). If a test saves assets, use
the `mcp_namespace` fixture.

For the few bridge-internal ops with no MCP tool, take the `bridge` fixture for
that one call and leave a comment (see `test_smoke.py::test_ping`).
```
