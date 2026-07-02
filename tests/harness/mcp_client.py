"""MCP-protocol client — drives the REAL product surface.

Unlike `bridge_client` (which speaks raw JSON to the C++ plugin on 55557, skipping
the Python tool layer), this connects to the running Python MCP server over its
streamable-HTTP endpoint (`http://127.0.0.1:8765/mcp`) and calls tools BY NAME with
their TOOL kwargs — exactly what an MCP client (Claude Code, etc.) does. So a test
through this path exercises: MCP protocol → `@mcp.tool()` layer (kwarg→bridge-param
mapping, defaults, multi-call orchestration, response shaping) → bridge → engine.

The MCP SDK client is async; pytest is sync. We run a private asyncio loop in a
background thread and marshal each call onto it, exposing a blocking `.call()`.
"""

from __future__ import annotations

import asyncio
import json
import threading
import time
from typing import Any, Dict, Optional

from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client


class MCPError(RuntimeError):
    """Transport/protocol failure talking to the MCP server."""


class MCPToolError(RuntimeError):
    """A tool call returned an error result (isError) or an error envelope."""

    def __init__(self, tool: str, payload: Any):
        self.tool = tool
        self.payload = payload
        super().__init__(f"{tool}: {payload}")


class MCPClient:
    """Blocking facade over the async MCP streamable-HTTP client."""

    def __init__(self, url: str = "http://127.0.0.1:8765/mcp"):
        self.url = url
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._thread: Optional[threading.Thread] = None
        self._session: Optional[ClientSession] = None
        self._session_cm = None
        self._stream_cm = None

    # ── lifecycle ────────────────────────────────────────────────────────────
    def connect(self, timeout: float = 60.0) -> "MCPClient":
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._loop.run_forever, daemon=True)
        self._thread.start()
        self._run(self._aconnect(), timeout)
        return self

    async def _aconnect(self) -> None:
        self._stream_cm = streamablehttp_client(self.url)
        read, write, _ = await self._stream_cm.__aenter__()
        self._session_cm = ClientSession(read, write)
        self._session = await self._session_cm.__aenter__()
        await self._session.initialize()

    def close(self) -> None:
        if self._loop is None:
            return
        try:
            self._run(self._aclose(), 15.0)
        except Exception:
            pass
        self._loop.call_soon_threadsafe(self._loop.stop)
        if self._thread:
            self._thread.join(timeout=5)

    async def _aclose(self) -> None:
        if self._session_cm is not None:
            await self._session_cm.__aexit__(None, None, None)
        if self._stream_cm is not None:
            await self._stream_cm.__aexit__(None, None, None)

    # ── calls ────────────────────────────────────────────────────────────────
    def list_tools(self) -> list[str]:
        res = self._run(self._session.list_tools(), 30.0)
        return [t.name for t in res.tools]

    def call(self, tool: str, arguments: Optional[Dict[str, Any]] = None,
             timeout: float = 180.0) -> Dict[str, Any]:
        """Call a tool by name with its kwargs; return the parsed result dict.
        Does NOT raise on a tool-level error envelope (use :meth:`expect`)."""
        return self._run(self._acall(tool, arguments or {}), timeout)

    # Alias so cross-transport helpers (harness.ops) work with either client.
    def command(self, tool: str, arguments: Optional[Dict[str, Any]] = None,
                timeout: float = 180.0) -> Dict[str, Any]:
        return self.call(tool, arguments, timeout)

    async def _acall(self, tool: str, arguments: Dict[str, Any]) -> Dict[str, Any]:
        res = await self._session.call_tool(tool, arguments)
        # FastMCP returns the tool's dict as structuredContent; older/text paths
        # carry it as JSON in the first text content block.
        data: Any = None
        structured = getattr(res, "structuredContent", None)
        if isinstance(structured, dict):
            # FastMCP wraps a non-model return under {"result": <dict>}; unwrap.
            data = structured.get("result", structured) if "result" in structured else structured
        if data is None:
            for c in getattr(res, "content", []) or []:
                text = getattr(c, "text", None)
                if text:
                    try:
                        data = json.loads(text)
                    except (ValueError, TypeError):
                        data = {"_text": text}
                    break
        if data is None:
            data = {}
        if getattr(res, "isError", False):
            raise MCPToolError(tool, data)
        return data

    def expect(self, tool: str, arguments: Optional[Dict[str, Any]] = None,
               timeout: float = 180.0) -> Dict[str, Any]:
        """Call a tool and require a non-error result. The tool layer normalizes
        bridge envelopes; success shows as status=='success' OR success!=False OR
        the absence of an error/status=='error'. Returns the result dict."""
        result = self.call(tool, arguments, timeout)
        status = result.get("status")
        if status == "error" or result.get("success") is False:
            raise MCPToolError(tool, result)
        # Many tools return the bridge's {status, result, error} envelope; unwrap
        # the inner result when present so call sites read the payload directly.
        inner = result.get("result")
        if status == "success" and isinstance(inner, dict):
            return inner
        return result

    def wait_ready_via_status(self, timeout: float = 60.0, poll: float = 1.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.is_ready():
                return True
            time.sleep(poll)
        return self.is_ready()

    def is_ready(self) -> bool:
        try:
            r = self.call("mcp_status", {}, timeout=10)
        except Exception:
            return False
        inner = r.get("result") if isinstance(r, dict) else None
        if isinstance(inner, dict):
            return bool(inner.get("ready"))
        return bool(r.get("ready"))

    # ── internals ────────────────────────────────────────────────────────────
    def _run(self, coro, timeout: float):
        if self._loop is None:
            raise MCPError("client not connected")
        fut = asyncio.run_coroutine_threadsafe(coro, self._loop)
        try:
            return fut.result(timeout)
        except Exception as e:  # surface as MCPError for uniform handling
            raise MCPError(str(e)) from e
