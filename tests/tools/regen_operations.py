#!/usr/bin/env python
"""Regenerate tests/harness/operations.py from the live server source.

Run from the repo root:  python tests/tools/regen_operations.py

The manifest is the set of distinct bridge command names dispatched via
send_command("...") anywhere under src/MCP/, plus the gate-bypass 'ping'. Keeping
it generated (not hand-maintained) means new operations show up as coverage
gaps automatically the next time someone regenerates + runs the guard test.
"""

import re
import pathlib

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = REPO_ROOT / "src" / "MCP"
OUT = REPO_ROOT / "tests" / "harness" / "operations.py"

_PAT = re.compile(r'send_command\(\s*"([a-zA-Z0-9_]+)"')


def discover_ops() -> list[str]:
    cmds: set[str] = {"ping"}
    for p in SRC.rglob("*.py"):
        cmds |= set(_PAT.findall(p.read_text(encoding="utf-8", errors="ignore")))
    return sorted(cmds)


def main() -> None:
    cmds = discover_ops()
    body = ['"""Authoritative manifest of every UnrealMCP bridge operation.',
            '',
            'Generated from the distinct send_command("...") dispatches in src/MCP/ plus the',
            "gate-bypass 'ping'. The coverage-guard test (integration/test_zz_coverage.py)",
            'asserts every name here is exercised by at least one integration test via the',
            '@covers(...) decorator. Regenerate with tests/tools/regen_operations.py.',
            '"""',
            '',
            'ALL_OPERATIONS = frozenset({']
    body += [f'    "{c}",' for c in cmds]
    body += ['})', '', f'assert len(ALL_OPERATIONS) == {len(cmds)}', '']
    OUT.write_text("\n".join(body), encoding="utf-8")
    print(f"wrote {OUT} with {len(cmds)} operations")


if __name__ == "__main__":
    main()
