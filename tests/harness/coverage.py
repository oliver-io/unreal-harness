"""Operation-coverage tracking.

Each integration test declares which bridge operation(s) it exercises with the
``@covers(...)`` decorator:

    from harness.coverage import covers

    @covers("actor_spawn")
    def test_actor_spawn(bridge): ...

The decorator records the op names on the function (handy for introspection) and
into a process-wide registry. But the authoritative coverage check is a STATIC
scan of the test sources (:func:`scan_covered_ops`), so it works even on partial
runs / -k filters where not every module is imported.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Iterable, Set

# Runtime registry (populated at import time as decorators execute).
COVERED: Set[str] = set()


def covers(*ops: str):
    """Mark a test as exercising one or more bridge operations."""
    COVERED.update(ops)

    def deco(fn):
        prev = set(getattr(fn, "_covers", ()))
        fn._covers = tuple(sorted(prev | set(ops)))
        return fn

    return deco


_COVERS_LITERAL = re.compile(r'covers\(\s*((?:"[a-zA-Z0-9_]+"\s*,?\s*)+)\)')
_STR = re.compile(r'"([a-zA-Z0-9_]+)"')


def scan_covered_ops(integration_dir: Path) -> Set[str]:
    """Statically collect every op named in a @covers(...) call under a dir.

    Source-of-truth for the coverage guard: independent of import side effects,
    so `pytest -k ...` or running a single file still yields the true covered
    set for the whole suite.
    """
    found: Set[str] = set()
    for p in sorted(integration_dir.glob("test_*.py")):
        if p.name == "test_zz_coverage.py":
            continue  # the guard references covers(...) in prose; don't scan itself
        text = p.read_text(encoding="utf-8", errors="ignore")
        for group in _COVERS_LITERAL.findall(text):
            found.update(_STR.findall(group))
    return found
