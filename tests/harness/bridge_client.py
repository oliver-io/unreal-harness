"""Minimal TCP/JSON client for the UnrealMCP C++ bridge (127.0.0.1:55557).

Deliberately a thin, dependency-free mirror of the production
``UnrealConnection`` wire contract in ``src/MCP/server.py`` — NOT a reuse of it.
Talking straight to the bridge keeps the integration tests focused on the
C++ command handlers and removes the FastMCP/HTTP layer as a variable. The wire
contract we mirror exactly:

  * one command per TCP connection: connect -> send -> recv-until-parse -> close
  * request  framing: a single UTF-8 JSON object ``{"type": <cmd>, "params": {...}}``
    with NO length prefix
  * response framing: a single UTF-8 JSON object, terminated by the peer closing
    the socket OR by the payload becoming parseable JSON, whichever comes first
  * envelope: ``{"status": "success"|"error", "result": {...}, "error": ...}``

(If you instead want to exercise the whole Python+HTTP stack end to end, point a
real MCP client at http://127.0.0.1:8765/mcp — see tests/README.md.)
"""

from __future__ import annotations

import json
import socket
import time
from typing import Any, Dict, Optional


class BridgeError(RuntimeError):
    """Transport-level failure (couldn't reach the bridge / bad response)."""


class CommandError(RuntimeError):
    """The bridge answered with ``status == "error"``. Carries the envelope."""

    def __init__(self, command: str, envelope: Dict[str, Any]):
        self.command = command
        self.envelope = envelope
        self.error_code = envelope.get("error_code")
        msg = envelope.get("error") or envelope.get("message") or "unknown error"
        super().__init__(f"{command}: {msg} [{self.error_code or 'no_code'}]")


class BridgeClient:
    """A connect-per-command client. Cheap to construct; holds no socket between
    calls (matching the production client's one-shot-per-connection model)."""

    BUFFER_SIZE = 8192

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 55557,
        default_timeout: float = 30.0,
        connect_timeout: float = 5.0,
    ):
        self.host = host
        self.port = port
        self.default_timeout = default_timeout
        self.connect_timeout = connect_timeout

    # ── core round-trip ──────────────────────────────────────────────────────
    def command(
        self,
        command_type: str,
        params: Optional[Dict[str, Any]] = None,
        timeout: Optional[float] = None,
    ) -> Dict[str, Any]:
        """Send one command, return the parsed response envelope. Raises
        BridgeError on transport failure; never raises on a ``status==error``
        envelope (use :meth:`expect` for that)."""
        timeout = self.default_timeout if timeout is None else timeout
        payload = json.dumps({"type": command_type, "params": params or {}}).encode("utf-8")

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(self.connect_timeout)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            try:
                sock.connect((self.host, self.port))
            except OSError as e:
                raise BridgeError(
                    f"could not connect to bridge at {self.host}:{self.port} "
                    f"(is the editor running?): {e}"
                ) from e

            sock.settimeout(timeout)
            try:
                sock.sendall(payload)
                raw = self._recv_until_json(sock, timeout)
            except OSError as e:
                # The bridge forcibly closes connections while the editor is
                # still booting (WinError 10054 / ECONNRESET). That's "not ready
                # yet", not a fatal error — surface it as BridgeError so callers
                # like is_ready()/wait_ready() treat it as a retry, not a crash.
                raise BridgeError(
                    f"{command_type}: transport error (editor booting or gone?): {e}"
                ) from e
        finally:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()

        try:
            return json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as e:
            raise BridgeError(f"{command_type}: malformed response: {e}") from e

    def _recv_until_json(self, sock: socket.socket, timeout: float) -> bytes:
        chunks: list[bytes] = []
        deadline = time.monotonic() + timeout
        while True:
            if time.monotonic() > deadline:
                raise BridgeError(f"timed out after {timeout:.0f}s waiting for response")
            try:
                chunk = sock.recv(self.BUFFER_SIZE)
            except socket.timeout as e:
                if chunks and self._parses(b"".join(chunks)):
                    return b"".join(chunks)
                raise BridgeError(f"recv timed out: {e}") from e
            if not chunk:  # peer closed
                data = b"".join(chunks)
                if data and self._parses(data):
                    return data
                raise BridgeError("connection closed before a complete response arrived")
            chunks.append(chunk)
            data = b"".join(chunks)
            if self._parses(data):
                return data

    @staticmethod
    def _parses(data: bytes) -> bool:
        try:
            json.loads(data.decode("utf-8"))
            return True
        except (UnicodeDecodeError, json.JSONDecodeError):
            return False

    # ── assertions / convenience ─────────────────────────────────────────────
    def expect(
        self,
        command_type: str,
        params: Optional[Dict[str, Any]] = None,
        timeout: Optional[float] = None,
    ) -> Dict[str, Any]:
        """Like :meth:`command` but raises CommandError unless ``status==success``.
        Returns the ``result`` object so call sites read naturally:

            result = bridge.expect("actor_spawn", {"class": "/Script/Engine.PointLight"})
        """
        resp = self.command(command_type, params, timeout)
        if resp.get("status") != "success":
            raise CommandError(command_type, resp)
        result = resp.get("result")
        return result if isinstance(result, dict) else {}

    def mcp_status(self) -> Dict[str, Any]:
        """Boot-gate probe. Answered on the bridge's network thread even mid-init."""
        return self.command("mcp_status", {}, timeout=self.connect_timeout)

    def is_ready(self) -> bool:
        try:
            resp = self.mcp_status()
        except BridgeError:
            return False
        result = resp.get("result") if isinstance(resp, dict) else None
        return bool(isinstance(result, dict) and result.get("ready"))

    def ping(self) -> Dict[str, Any]:
        return self.command("ping", {}, timeout=self.connect_timeout)

    def wait_ready(self, timeout: float, poll: float = 1.0) -> bool:
        """Poll mcp_status until ``result.ready`` is true. Returns False on timeout."""
        deadline = time.monotonic() + timeout
        delay = min(poll, 0.5)
        while time.monotonic() < deadline:
            if self.is_ready():
                return True
            time.sleep(delay)
            delay = min(delay * 1.5, 3.0)
        return self.is_ready()
