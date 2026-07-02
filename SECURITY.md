# Security Policy

## Trust model: the local machine is the boundary

This project is a **workbench** that lets an AI agent (Claude Code) and its operator
drive a live Unreal Editor. Its trust boundary is **your local machine**, and it is
designed to run with **your own privileges**.

Two listeners make up the harness, and both bind loopback by default:

| Listener            | Default bind      | Purpose                                        |
| ------------------- | ----------------- | ---------------------------------------------- |
| MCP HTTP server     | `127.0.0.1:8765`  | the MCP client talks to the server here        |
| Unreal editor bridge| `127.0.0.1:55557` | the server forwards JSON commands to the editor|

Neither listener is authenticated. By design, the bridge **executes operator-authored
code** — typed tool calls, and the deliberate `editor_console_exec` escape hatch (including
its `py` path) run arbitrary console/Python inside your editor. Treat anything that can
reach these ports as able to act as you, with your permissions, on your machine and project.

Remote code execution is therefore **not a vulnerability in this model** — running the
operator's code is the feature. The security posture is entirely about **who can reach the
ports.**

### Keep it local

- **Do not bind the listeners to a routable interface.** `UNREAL_MCP_HOST` and
  `UNREAL_BRIDGE_HOST` can be overridden (e.g. to `0.0.0.0`); doing so exposes an
  unauthenticated, code-executing surface to your network. Leave them on `127.0.0.1`.
- If you must reach the server from another host, put it behind an authenticated tunnel
  (SSH, a reverse proxy with auth, a VPN) rather than binding off-loopback.
- The harness is intended for a single trusted operator on the local machine, not as a
  multi-tenant or internet-facing service.

## Reporting a vulnerability

If you find a security issue that falls **outside** this local-trust model — for example a
default that unexpectedly binds off-loopback, a path that escapes the intended sandbox, or
a dependency advisory — please report it privately:

- Open a **GitHub Security Advisory** on this repository ("Security" tab →
  "Report a vulnerability"), or
- Open a regular issue for non-sensitive hardening suggestions.

Please do not open a public issue for something exploitable until it has been addressed.

## A note from the author:
If you aren't creating assets and et cetera yourself, you are probably downloading untrustable binary data formats for
a source-available engine that basically does RCE by design.  Luckily, it's all fun and games.  I wouldn't host the UE
*editor* in prod, though.  The games should be hosted in environments that are sacrificial and isolated.

PRECISELY NOTHING ABOUT THE UNREAL EDITOR IS SECURE.  I run this most of the time with `--dangerously-skip-permission`.
Buyer beware, caveat emptor, no liability for damages you incur on yourself.  Don't gamble with LLMs you can't afford
doing something wacky.