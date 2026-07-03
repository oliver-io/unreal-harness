"""Launch, await, and tear down a live Unreal Editor for the integration suite.

Two launch modes:

  * ``headless`` (default) — ``UnrealEditor-Cmd.exe <uproject> -nullrhi ...``.
    Boots the FULL editor (so FEditorDelegates::OnEditorInitialized fires and the
    UnrealMCP boot gate opens) but with a null render backend: no GPU, no
    rendered window. Correct for everything except pixel-dependent ops.

  * ``gui`` — ``UnrealEditor.exe <uproject> ...`` with real RHI and a real
    window. Required for screenshot / thumbnail / render tests.

Lifecycle: ensure the plugin is linked into the project -> (optionally) build the
editor target -> spawn the editor, teeing its stdout to a log file -> poll
``mcp_status`` until interactive -> yield -> graceful QUIT_EDITOR, then kill.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import List, Optional

from . import config
from . import _cleanup
from .bridge_client import BridgeClient


class EditorLaunchError(RuntimeError):
    pass


def ensure_plugin_linked() -> None:
    """Make sure <project>/Plugins/UnrealMCP resolves to src/Plugin/UnrealMCP.

    Uses a directory junction (Windows) / symlink (POSIX) so plugin edits in the
    repo are live and we never copy the C++ source. No-op if already present.
    """
    dst = config.plugin_dest()
    src = config.PLUGIN_SRC
    if not src.is_dir():
        raise EditorLaunchError(f"plugin source not found at {src}")
    if dst.exists():
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    if sys.platform.startswith("win"):
        # mklink /J needs cmd.exe; works without admin for junctions.
        subprocess.run(
            ["cmd.exe", "/c", "mklink", "/J", str(dst), str(src)],
            check=True, capture_output=True, text=True,
        )
    else:
        os.symlink(src, dst, target_is_directory=True)


def is_built() -> bool:
    return config.editor_module_dll().exists()


def build_editor(log_path: Optional[Path] = None) -> None:
    """Compile the editor target (project module + all enabled plugins) via UBT.

    This is what makes the C++ UnrealMCP plugin loadable. First build is slow
    (minutes); subsequent builds are incremental thanks to the gitignored
    Intermediate/ + Binaries/.
    """
    cmd: List[str] = [
        str(config.build_script()),
        config.editor_target(),
        "Win64" if sys.platform.startswith("win") else ("Mac" if sys.platform == "darwin" else "Linux"),
        "Development",
        f"-Project={config.uproject_path()}",
        "-WaitMutex",
    ]
    _run_streamed(cmd, log_path, phase="build")


class EditorSession:
    """Owns one editor process. Use as a context manager or call start()/stop()."""

    def __init__(
        self,
        mode: str = "headless",
        build: str = "auto",          # auto | always | never
        boot_timeout: Optional[float] = None,
        log_dir: Optional[Path] = None,
    ):
        if mode not in ("headless", "gui"):
            raise ValueError(f"mode must be 'headless' or 'gui', got {mode!r}")
        if build not in ("auto", "always", "never"):
            raise ValueError(f"build must be auto|always|never, got {build!r}")
        self.mode = mode
        self.build = build
        self.boot_timeout = boot_timeout if boot_timeout is not None else config.BOOT_TIMEOUT_S
        self.log_dir = log_dir or (config.project_dir() / "Saved" / "MCPTestLogs")
        self.proc: Optional[subprocess.Popen] = None
        self._log_fh = None
        self.bridge = BridgeClient(config.BRIDGE_HOST, config.BRIDGE_PORT)

    # ── lifecycle ────────────────────────────────────────────────────────────
    def start(self) -> "EditorSession":
        ensure_plugin_linked()
        _free_bridge_port_from_stray_editors(self.bridge)
        self.log_dir.mkdir(parents=True, exist_ok=True)

        if self.build == "always" or (self.build == "auto" and not is_built()):
            build_editor(self.log_dir / "build.log")
        elif not is_built():
            raise EditorLaunchError(
                f"editor target not built ({config.editor_module_dll()} missing) and "
                "build=never. Build it first or pass --ue-build=auto."
            )

        self._launch()
        self._await_ready()
        return self

    def restart(self) -> "EditorSession":
        """Relaunch a (likely crashed) editor without rebuilding. Used by the
        session to recover after an op fatals the process — keeps one crash from
        cascading into every subsequent test."""
        self._terminate(grace=10.0)
        self._launch()
        self._await_ready()
        return self

    def alive_and_ready(self) -> bool:
        if self.proc is not None and self.proc.poll() is not None:
            return False
        return self.bridge.is_ready()

    def stop(self) -> None:
        # Graceful first: ask the editor to quit so it flushes cleanly. Only
        # works once the gate is open; ignore failures and fall back to kill.
        if self.proc and self.proc.poll() is None:
            try:
                self.bridge.command("editor_console_exec", {"command": "QUIT_EDITOR"}, timeout=5)
            except Exception:
                pass
            self._terminate(grace=20.0)
        if self._log_fh:
            try:
                self._log_fh.close()
            finally:
                self._log_fh = None

    # ── internals ────────────────────────────────────────────────────────────
    def _launch(self) -> None:
        uproject = str(config.uproject_path())
        editor_log = self.log_dir / f"editor_{self.mode}.log"
        # -AutoDeclinePackageRecovery: a prior kill/crash can leave auto-save
        # recovery data that pops a modal "Restore Packages" dialog on startup,
        # which would hang an unattended boot. Decline it (the test project has no
        # work worth recovering).
        common = [
            "-stdout", "-FullStdOutLogOutput", f"-AbsLog={editor_log}",
            "-AutoDeclinePackageRecovery",
        ]

        if self.mode == "headless":
            exe = config.editor_cmd_exe()
            args = [
                str(exe), uproject,
                "-nullrhi", "-nosound",
                "-unattended", "-nopause", "-nosplash",
            ] + common
        else:  # gui — real RHI + window for render/screenshot tests
            exe = config.editor_gui_exe()
            args = [str(exe), uproject] + common

        if not Path(exe).exists():
            raise EditorLaunchError(f"editor binary not found: {exe}")

        # Tee the process's own stdout too (separate from -AbsLog, which is the
        # engine's structured log) so a launch that dies before logging is visible.
        self._log_fh = open(self.log_dir / f"editor_{self.mode}.stdout.log", "w", encoding="utf-8")
        self.proc = _cleanup.track(subprocess.Popen(
            args,
            stdout=self._log_fh,
            stderr=subprocess.STDOUT,
            cwd=str(config.engine_root()),
        ))

    def _await_ready(self) -> None:
        deadline = time.monotonic() + self.boot_timeout
        while time.monotonic() < deadline:
            if self.proc and self.proc.poll() is not None:
                raise EditorLaunchError(
                    f"editor exited during boot (code {self.proc.returncode}); "
                    f"see {self.log_dir}"
                )
            if self.bridge.is_ready():
                return
            time.sleep(1.0)
        self._terminate(grace=10.0)
        raise EditorLaunchError(
            f"editor did not become interactive within {self.boot_timeout:.0f}s "
            f"(mcp_status.ready never true); see {self.log_dir}"
        )

    def _terminate(self, grace: float) -> None:
        if not self.proc:
            return
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=grace)
            except subprocess.TimeoutExpired:
                pass
        # Sweep the whole tree even on the clean-exit path. TerminateProcess (and
        # SIGTERM) reap the editor but NOT its child workers — ShaderCompileWorker,
        # CrashReportClient, EpicWebHelper — which otherwise linger and, for the
        # headless -Cmd editor, can keep cross-talking on the bridge port. The
        # tree-kill is idempotent if the tree is already gone.
        if sys.platform.startswith("win"):
            subprocess.run(
                ["taskkill", "/F", "/T", "/PID", str(self.proc.pid)],
                capture_output=True,
            )
        elif self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=grace)
            except subprocess.TimeoutExpired:
                pass
        self.proc = None

    def __enter__(self) -> "EditorSession":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()


def _free_bridge_port_from_stray_editors(bridge) -> None:
    """The bridge port must belong to the editor WE launch. Anything already
    listening there — a zombie headless test editor OR another project's live
    editor — is machine-level state the test run owns (docs/TESTING.md): its
    cross-talk on the port corrupts the new session, and attaching to it is how
    test mutations end up inside a real project. Stop the owning process tree
    (precise: the port owner, not a blanket image-name sweep) so our editor
    owns the port. Best-effort + Windows-only (the test platform)."""
    if not _port_in_use(bridge.host, bridge.port):
        return
    if not sys.platform.startswith("win"):
        return
    for pid in _port_owner_pids(bridge.port):
        print(
            f"[harness] bridge port {bridge.port} held by pid {pid} — "
            "stopping it so the test editor owns the port",
            flush=True,
        )
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], capture_output=True)
    # Wait for the stray to FULLY release the port (TIME_WAIT included) before we
    # launch — otherwise the new editor's listener bind races the dying one and
    # the boot gate may never open. Up to ~15s.
    for _ in range(60):
        if not _port_in_use(bridge.host, bridge.port):
            time.sleep(0.5)  # small settle margin after the socket frees
            return
        time.sleep(0.25)


def _port_owner_pids(port: int) -> List[int]:
    """PIDs of processes LISTENING on the port (Windows netstat parse)."""
    out = subprocess.run(
        ["netstat", "-ano", "-p", "TCP"], capture_output=True, text=True
    ).stdout
    pids = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 5 and parts[3] == "LISTENING" and parts[1].endswith(f":{port}"):
            try:
                pids.add(int(parts[4]))
            except ValueError:
                pass
    return sorted(pids)


def _port_in_use(host: str, port: int) -> bool:
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.5)
    try:
        return s.connect_ex((host, port)) == 0
    finally:
        s.close()


def reset_test_content() -> None:
    """Delete the on-disk scratch namespace (Content/__MCPTest__). A full reset
    for any test that saved assets. Safe to call with the editor stopped."""
    d = config.test_content_dir()
    if d.exists():
        shutil.rmtree(d, ignore_errors=True)


def _run_streamed(cmd: List[str], log_path: Optional[Path], phase: str) -> None:
    if log_path:
        log_path.parent.mkdir(parents=True, exist_ok=True)
    with open(log_path, "w", encoding="utf-8") if log_path else _nullcontext() as fh:
        proc = subprocess.run(
            cmd,
            stdout=fh if fh else None,
            stderr=subprocess.STDOUT,
            text=True,
        )
    if proc.returncode != 0:
        raise EditorLaunchError(
            f"{phase} failed (exit {proc.returncode}): {' '.join(cmd)}"
            + (f"\nsee {log_path}" if log_path else "")
        )


class _nullcontext:
    def __enter__(self):
        return None

    def __exit__(self, *exc):
        return False
