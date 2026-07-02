"""Process-leak safety net for the test harness.

Session-scoped fixtures tear down their editor / MCP-server on normal completion
and on most failures. But a hard interrupt (Ctrl-C, a fatal in collection, an
unhandled error before `yield`) can skip that teardown and orphan a launched
process — which then holds the bridge (55557) or MCP (8765) port and corrupts
the *next* run.

Every process the harness spawns registers here via `track()`. A single
`atexit` hook tree-kills any that are still alive at interpreter exit. This is a
belt, not the primary path: the fixtures' own graceful stop() still runs first;
sweeping an already-dead PID is a harmless no-op. atexit fires on normal exit and
on SIGINT (KeyboardInterrupt) — the interrupts a developer actually hits.
"""

from __future__ import annotations

import atexit
import subprocess
import sys
import weakref

# weakrefs so tracking a proc never keeps it (or its Popen wrapper) alive.
_tracked: "list[weakref.ref]" = []


def track(proc: subprocess.Popen) -> subprocess.Popen:
    """Register a spawned process for best-effort tree-kill at interpreter exit."""
    _tracked.append(weakref.ref(proc))
    return proc


def _tree_kill(proc: subprocess.Popen) -> None:
    try:
        if proc.poll() is not None:
            return
        if sys.platform.startswith("win"):
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                           capture_output=True)
        else:
            proc.kill()
    except Exception:
        pass


@atexit.register
def _sweep() -> None:
    for ref in _tracked:
        proc = ref()
        if proc is not None:
            _tree_kill(proc)
