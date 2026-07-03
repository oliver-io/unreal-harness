#!/usr/bin/env python
"""Regenerate tests/harness/operations.py from the live server + plugin sources.

Run from the repo root:  python tests/tools/regen_operations.py
Requires: python 3 + bun (the Bun server's own runtime).

The manifest is the set of BRIDGE operations — wire command names that the Bun
server can actually send AND the C++ plugin actually dispatches:

    manifest = (server-sendable wire names  ∩  C++ dispatch keys)  ∪  {"ping"}

where the two sides are derived from source, never hand-maintained:

  * Server-sendable wire names — the union of:
      1. every canonical tool name from ``buildRegistry()`` in
         ``src/server/src/register.ts`` (enumerated via a ``bun -e`` one-liner;
         for a ``bridgeTool`` spec the wire name defaults to the tool name —
         ``spec.command ?? spec.name``, see ``src/server/src/domains/_shared.ts``);
      2. every ``command: "..."`` override in a ``bridgeTool`` spec (legacy wire
         names, e.g. server tool ``statetree_node_add`` sends ``st_add_node``);
      3. every ``sendCommand("...")`` literal inside ``defineTool`` custom
         handlers and composites under ``src/server/src/``.
  * C++ dispatch keys — every ``CommandType == TEXT("...")`` comparison in
    ``src/Plugin/UnrealMCP/Source/UnrealMCP/Private/`` (MCPBridge.cpp plus the
    per-domain sub-dispatchers in ``Commands/*.cpp``).

The intersection is the principled rule:

  * Server-local tools (``catalog_*``, ``code_*``, ``result_read``,
    ``build_status``, ``video_analyze``, ``pie_analyze``, ``editor_read_logs``,
    ``editor_build_game_target``, composites like ``actor_spawn_physics``, …)
    never hit the bridge, so they have no C++ dispatch key and are excluded —
    their coverage is a bun-side concern, tracked separately.
  * Dead legacy C++ dispatch keys unreachable from any server tool
    (``add_conduit``, ``spawn_actor``, ``merge_bones_*``, …) are excluded —
    nothing can send them, so they must not force test coverage.
  * ``ping`` is added explicitly: it is the gate-bypass health probe the test
    harness itself sends, even though no registered tool forwards it.

Keeping the manifest generated means new operations show up as coverage gaps
automatically the next time someone regenerates + runs the guard test
(integration/test_zz_coverage.py).
"""

import json
import pathlib
import re
import shutil
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SERVER_DIR = REPO_ROOT / "src" / "server"
SERVER_SRC = SERVER_DIR / "src"
PLUGIN_SRC = REPO_ROOT / "src" / "Plugin" / "UnrealMCP" / "Source" / "UnrealMCP" / "Private"
OUT = REPO_ROOT / "tests" / "harness" / "operations.py"

_TS_COMMAND_OVERRIDE = re.compile(r'\bcommand:\s*"([a-zA-Z0-9_]+)"')
_TS_SEND_COMMAND = re.compile(r'\bsendCommand\(\s*"([a-zA-Z0-9_]+)"')
_CPP_DISPATCH = re.compile(r'CommandType\s*==\s*TEXT\("([a-zA-Z0-9_]+)"\)')

_BUN_ENUMERATE = (
    'import { buildRegistry } from "./src/register.ts";'
    "console.log(JSON.stringify(buildRegistry().all().map((t) => t.name)));"
)


def registry_tool_names() -> set[str]:
    """Canonical tool names from the live Bun registry (self-updating)."""
    bun = shutil.which("bun")
    if not bun:
        sys.exit("bun is required on PATH to enumerate the tool registry")
    proc = subprocess.run(
        [bun, "-e", _BUN_ENUMERATE],
        cwd=SERVER_DIR,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        sys.exit(f"bun registry enumeration failed:\n{proc.stderr}")
    names = json.loads(proc.stdout.strip().splitlines()[-1])
    return set(names)


def server_sendable() -> set[str]:
    """Every wire name the server source can put on the socket."""
    names = registry_tool_names()  # bridgeTool default: wire == tool name
    for p in SERVER_SRC.rglob("*.ts"):
        text = p.read_text(encoding="utf-8", errors="ignore")
        names |= set(_TS_COMMAND_OVERRIDE.findall(text))  # command: overrides
        names |= set(_TS_SEND_COMMAND.findall(text))  # defineTool literals
    return names


def cpp_dispatch_keys() -> set[str]:
    """Every wire name the C++ plugin dispatches on."""
    keys: set[str] = set()
    for p in PLUGIN_SRC.rglob("*.cpp"):
        keys |= set(_CPP_DISPATCH.findall(p.read_text(encoding="utf-8", errors="ignore")))
    return keys


def discover_ops() -> list[str]:
    return sorted((server_sendable() & cpp_dispatch_keys()) | {"ping"})


def main() -> None:
    cmds = discover_ops()
    body = [
        '"""Authoritative manifest of every UnrealMCP bridge operation.',
        "",
        "GENERATED — do not edit by hand; regenerate with tests/tools/regen_operations.py.",
        "",
        "Derivation: (wire names the Bun server can send — registry tool names,",
        'bridgeTool `command:` overrides, and sendCommand("...") literals under',
        "src/server/src/) INTERSECTED with the C++ dispatch keys",
        '(CommandType == TEXT("...") under src/Plugin/UnrealMCP/), plus the',
        "gate-bypass 'ping'. Server-local tools and dead legacy C++ names are",
        "excluded by construction. The coverage-guard test",
        "(integration/test_zz_coverage.py) asserts every name here is exercised by",
        "at least one integration test via the @covers(...) decorator.",
        '"""',
        "",
        "ALL_OPERATIONS = frozenset({",
    ]
    body += [f'    "{c}",' for c in cmds]
    body += ["})", "", f"assert len(ALL_OPERATIONS) == {len(cmds)}", ""]
    OUT.write_text("\n".join(body), encoding="utf-8")
    print(f"wrote {OUT} with {len(cmds)} operations")


if __name__ == "__main__":
    main()
