"""Launch / await / stop the Bun MCP server (the product under test).

`bun run mcp` from the repo root (where package.json lives; the server source is
src/server) boots the streamable-http server at 127.0.0.1:8765/mcp and forwards
to the editor bridge on 55557. The harness owns this process so the MCP-path
tests have a real server to talk to.

Lifecycle correctness (learned the hard way): a server left listening on 8765 by
a previous run carries that run's *stale tool catalog*, and silently reusing it
makes tests pass/fail against code that is no longer on disk. So unless we're in
attach mode, start() kills any pre-existing listener and launches a fresh server
reflecting the current source. And because `bun run` (under a shell on Windows)
spawns the real server as a grandchild, stop() tree-kills — a plain terminate()
on the wrapper leaves the grandchild bound to 8765.
"""

from __future__ import annotations

import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

from . import config
from . import _cleanup

# package.json (and the `mcp` script) live at the repo root.
SERVER_DIR = config.REPO_ROOT
MCP_HOST = "127.0.0.1"
MCP_PORT = 8765


def _port_listening(host: str, port: int) -> bool:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.5)
    try:
        return s.connect_ex((host, port)) == 0
    finally:
        s.close()


def _pids_on_port(port: int) -> list[int]:
    """PIDs with a LISTEN socket on `port`, dependency-free (netstat / lsof)."""
    pids: set[int] = set()
    try:
        if sys.platform.startswith("win"):
            out = subprocess.run(["netstat", "-ano", "-p", "TCP"],
                                  capture_output=True, text=True).stdout
            for line in out.splitlines():
                parts = line.split()
                if len(parts) >= 5 and parts[0] == "TCP" and "LISTENING" in parts \
                        and parts[1].endswith(f":{port}"):
                    pids.add(int(parts[-1]))
        else:
            out = subprocess.run(["lsof", "-tiTCP:%d" % port, "-sTCP:LISTEN"],
                                  capture_output=True, text=True).stdout
            pids = {int(p) for p in out.split()}
    except (OSError, ValueError):
        pass
    return sorted(pids)


def _kill_tree(pid: int) -> None:
    if sys.platform.startswith("win"):
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], capture_output=True)
    else:
        subprocess.run(["kill", "-9", str(pid)], capture_output=True)


def _free_port(port: int, timeout: float = 5.0) -> None:
    """Tree-kill anything listening on `port` and wait for it to release."""
    for pid in _pids_on_port(port):
        _kill_tree(pid)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and _port_listening(MCP_HOST, port):
        time.sleep(0.2)


class MCPServer:
    def __init__(self, log_dir: Optional[Path] = None, boot_timeout: float = 90.0):
        self.log_dir = log_dir or (config.project_dir() / "Saved" / "MCPTestLogs")
        self.boot_timeout = boot_timeout
        self.proc: Optional[subprocess.Popen] = None
        self._log_fh = None

    def start(self, fresh: bool = True) -> "MCPServer":
        if _port_listening(MCP_HOST, MCP_PORT):
            if not fresh:
                # Attach mode: deliberately reuse the running (dev) server.
                return self
            # Owned mode: a listener here is a stale leak from a prior run whose
            # catalog won't match the code under test. Kill it and launch fresh.
            _free_port(MCP_PORT)
            if _port_listening(MCP_HOST, MCP_PORT):
                raise RuntimeError(
                    f"could not free {MCP_HOST}:{MCP_PORT} (a server is wedged there); "
                    "kill it manually and re-run")
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self._log_fh = open(self.log_dir / "mcp_server.log", "w", encoding="utf-8")
        self.proc = _cleanup.track(subprocess.Popen(
            ["bun", "run", "mcp"],
            cwd=str(SERVER_DIR),
            stdout=self._log_fh,
            stderr=subprocess.STDOUT,
            shell=sys.platform.startswith("win"),  # find bun.exe on PATH on Windows
        ))
        deadline = time.monotonic() + self.boot_timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(
                    f"MCP server exited during startup (code {self.proc.returncode}); "
                    f"see {self.log_dir / 'mcp_server.log'}")
            if _port_listening(MCP_HOST, MCP_PORT):
                return self
            time.sleep(0.5)
        self.stop()
        raise RuntimeError(f"MCP server did not bind {MCP_HOST}:{MCP_PORT} within "
                           f"{self.boot_timeout:.0f}s")

    def stop(self) -> None:
        # `uv run` (under a shell on Windows) makes the real server a grandchild,
        # so terminate()/kill() on self.proc alone leaves it bound to 8765. Always
        # tree-kill by PID. Then, belt-and-suspenders, free the port in case the
        # tree-kill missed a re-parented child.
        if self.proc and self.proc.poll() is None:
            _kill_tree(self.proc.pid)
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                pass
        if self.proc is not None:
            _free_port(MCP_PORT, timeout=3.0)
        self.proc = None
        if self._log_fh:
            try:
                self._log_fh.close()
            finally:
                self._log_fh = None
