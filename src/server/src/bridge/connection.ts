/**
 * TCP client to the Unreal editor's C++ bridge — the Bun port of
 * `src/MCP/server.py::UnrealConnection`.
 *
 * Behaviour preserved exactly:
 *   - **Connection-per-command**: open → send one JSON command → read the reply
 *     → close. No long-lived socket (the bridge expects this framing).
 *   - **Read-until-parseable framing**: the bridge writes one JSON object and
 *     relies on the reader to detect the boundary by attempting to parse the
 *     accumulated buffer. There is no length prefix. Partial UTF-8 / partial
 *     JSON just means "keep reading".
 *   - **Boot gate**: real commands wait until the editor reports interactive
 *     (`mcp_status.ready == true`); `ping`/`mcp_status` bypass. A command that
 *     arrives mid-boot PENDS here instead of crashing the editor.
 *   - **Retry with exponential backoff** on transport errors.
 *   - **Large-op timeouts** for known slow commands.
 */

import { config } from "../config.ts";
import { envelopeError, isRecord, type Envelope } from "./envelope.ts";
import { buildInProgress } from "../build/lock.ts";

/**
 * A transport failure that occurred AFTER the command payload was written to the
 * socket. Such a command may already be executing in the editor, so it must NOT be
 * retried (re-sending a non-idempotent mutator duplicates it). See GAP-003.
 */
class PostSendError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "PostSendError";
  }
}

const MAX_RETRIES = 3;
const BASE_RETRY_DELAY_MS = 500;
const MAX_RETRY_DELAY_MS = 5000;
const CONNECT_TIMEOUT_MS = 10_000;
const DEFAULT_RECV_TIMEOUT_MS = 30_000;
const LARGE_OP_RECV_TIMEOUT_MS = 300_000;

// Boot gate (see docs/DEBUGGING.md). The bridge's TCP listener binds during
// editor subsystem init — it accepts connections long before the editor is
// interactive. Dispatching a real command into that window crashes startup.
const READINESS_TIMEOUT_MS = 120_000;
const READINESS_POLL_BASE_MS = 500;
const READINESS_POLL_MAX_MS = 3_000;

/** Commands that bypass the boot gate: the probe itself + the liveness check. */
const GATE_BYPASS = new Set(["mcp_status", "ping"]);

// Commands that need the long receive timeout (canonical wire names). A miss
// only costs the default 30s timeout instead of 300s.
const LARGE_OPERATION_COMMANDS = new Set([
  "material_get_available",
  "asset_list",
  "asset_save",
  "asset_fixup_redirectors",
  "asset_bake_dynamic_to_static_mesh",
  "asset_import_mesh",
  "asset_textures_import",
  "editor_build_game_target",
  "pie_start",
  "pie_stop",
  "material_read",
  "material_read_instance",
  "material_instance_set_parameter",
  "material_reparent_instance",
  "editor_live_coding_compile",
]);

// GAP-060: editor_live_coding_compile no longer blocks the bridge. The editor
// returns a `live_coding_in_progress` ticket immediately (the compile runs on the
// editor game thread) and stays responsive to mcp_status. We drive the compile to
// completion here: kick → poll mcp_status until it clears → retrieve the result.
const LIVE_CODING_COMMAND = "editor_live_coding_compile";
const LIVE_CODING_POLL_MS = 1_500;
const LIVE_CODING_DRIVE_TIMEOUT_MS = 600_000; // 10 min ceiling (matches the old C++ wait)

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

/** True if an envelope is the live-coding "still compiling" ticket. */
function isLiveCodingTicket(env: Envelope): boolean {
  const result = isRecord(env) ? env.result : undefined;
  return isRecord(result) && result.live_coding_in_progress === true;
}

/** Read mcp_status's live_coding_in_progress flag (undefined if absent/unreachable). */
function liveCodingInProgress(env: Envelope): boolean | undefined {
  const result = isRecord(env) ? env.result : undefined;
  if (isRecord(result) && typeof result.live_coding_in_progress === "boolean") {
    return result.live_coding_in_progress;
  }
  return undefined;
}

const recvTimeoutFor = (command: string): number =>
  LARGE_OPERATION_COMMANDS.has(command)
    ? LARGE_OP_RECV_TIMEOUT_MS
    : DEFAULT_RECV_TIMEOUT_MS;

export class UnrealConnection {
  // Sticky boot-gate cache: once the editor reports interactive we stop probing
  // (zero steady-state overhead). Reset whenever a transport error suggests the
  // editor went away, so a restart re-arms the gate.
  private editorReady = false;

  constructor(
    private readonly host: string = config.bridgeHost,
    private readonly port: number = config.bridgePort,
  ) {}

  /**
   * Send a command and return the bridge envelope. Real commands pass the boot
   * gate first; transport failures retry with backoff. Never throws — transport
   * problems are returned as error envelopes, matching the Python contract.
   */
  async sendCommand(
    command: string,
    params: Record<string, unknown> = {},
  ): Promise<Envelope> {
    if (!GATE_BYPASS.has(command)) {
      const gate = await this.awaitEditorReady();
      if (gate === "engine_unavailable") {
        // Contextualize: if a C++ build is running, the editor is SUPPOSED to be
        // down (a build recompiles with the editor closed). Say so explicitly so
        // agents don't mistake it for a crashed session.
        const build = buildInProgress();
        if (build) {
          return envelopeError(
            `Editor not reachable on ${this.host}:${this.port} because a C++ build ` +
              `is in progress: ${build.label} (pid ${build.pid}` +
              `${build.target ? `, target ${build.target}` : ""}), started ` +
              `${Math.round(build.held_ms / 1000)}s ago. This is EXPECTED during a ` +
              `build, NOT a crashed session — the editor returns once the build finishes.`,
            {
              code: "engine_busy",
              hint:
                "Poll build_status until build.in_progress is false (and the editor " +
                "is relaunched), then retry. Non-editor work can proceed meanwhile.",
            },
          );
        }
        return envelopeError(
          `Failed to connect to Unreal Engine — editor not running or not ` +
            `reachable on ${this.host}:${this.port}.`,
        );
      }
      if (gate === "timeout") {
        return envelopeError(
          `Editor still initializing after ${READINESS_TIMEOUT_MS / 1000}s; ` +
            `command '${command}' was not dispatched.`,
          {
            code: "editor_not_ready",
            hint:
              "The editor had not finished booting. Retry once it is " +
              "interactive (mcp_status result.ready == true).",
          },
        );
      }
    }

    // GAP-060: the live-coding compile is non-blocking on the wire — drive it
    // (kick → poll mcp_status → retrieve) instead of one long round-trip. The
    // `__lc_internal` guard lets the driver issue raw kick/retrieve calls through
    // dispatch() without recursing back into the driver.
    if (command === LIVE_CODING_COMMAND && params.__lc_internal !== true) {
      return this.driveLiveCodingCompile(params);
    }

    return this.dispatch(command, params);
  }

  /**
   * The transport retry loop: up to MAX_RETRIES connect-phase retries, with the
   * GAP-003 post-send non-retry rule. One open→send→read→close per attempt.
   * Boot gate is the caller's responsibility (sendCommand handles it).
   */
  private async dispatch(
    command: string,
    params: Record<string, unknown>,
  ): Promise<Envelope> {
    let lastError = "";
    for (let attempt = 0; attempt <= MAX_RETRIES; attempt++) {
      try {
        return await this.sendCommandOnce(command, params);
      } catch (err) {
        // GAP-003: only CONNECT-phase failures (the payload never reached the editor)
        // are safe to retry. Once the payload is on the wire, the editor may already be
        // executing the command — re-sending a non-idempotent mutator (e.g.
        // asset_import_mesh) duplicates it and has crashed the editor (TaskGraph
        // RecursionGuard). A post-send failure (recv timeout, socket drop after send)
        // surfaces as an error envelope instead of a retry.
        if (err instanceof PostSendError) {
          this.editorReady = false;
          return envelopeError(
            `Command '${command}' did not return a response in time (${err.message}). ` +
              `The payload was sent and may still be executing in the editor — it was NOT ` +
              `retried to avoid duplicate execution of a non-idempotent command.`,
            {
              code: "engine_busy",
              hint:
                "The editor did not respond before the receive timeout and the command was not retried " +
                "(it may still be executing). For heavy ops (imports, builds) it may have succeeded despite " +
                "the timeout — verify editor state before re-issuing. Reads can be safely re-called.",
            },
          );
        }
        lastError = err instanceof Error ? err.message : String(err);
        this.editorReady = false; // transport failed → re-arm the gate
        if (attempt < MAX_RETRIES) {
          await sleep(backoff(attempt));
        }
      }
    }
    return envelopeError(
      `Command failed after ${MAX_RETRIES + 1} attempts: ${lastError}`,
    );
  }

  /**
   * Drive a non-blocking live-coding compile to completion (GAP-060). The editor
   * kicks the compile onto its game thread and answers immediately with a
   * `live_coding_in_progress` ticket, staying responsive to mcp_status throughout.
   * We poll mcp_status until the compile clears, then collect the real result with
   * a `poll:true` retrieval — returning the SAME final envelope a caller saw before
   * (success/NoChanges/failure), so the tool surface is unchanged.
   */
  private async driveLiveCodingCompile(
    params: Record<string, unknown>,
  ): Promise<Envelope> {
    const kick = await this.dispatch(LIVE_CODING_COMMAND, {
      ...params,
      __lc_internal: true,
    });
    // If the kick was not a "compiling" ticket, it's a transport error envelope
    // (editor unreachable) or an immediate result — return it as-is.
    if (!isLiveCodingTicket(kick)) return kick;

    const deadline = Date.now() + LIVE_CODING_DRIVE_TIMEOUT_MS;
    while (Date.now() < deadline) {
      await sleep(LIVE_CODING_POLL_MS);
      const status = await this.dispatch("mcp_status", {});
      // mcp_status is answered purely on the editor's server thread, so a
      // transport error here (after dispatch's own retries) means the editor
      // actually went away mid-compile — surface it instead of polling forever.
      if (status.status === "error") {
        return envelopeError(
          "Editor became unreachable during a live-coding compile — it may have " +
            "crashed (a failed Live Coding patch can take the editor down). " +
            `Last status error: ${typeof status.error === "string" ? status.error : "unknown"}`,
          { code: "engine_busy", hint: "Check the editor process and MCP_Unified.log (LIVECODING)." },
        );
      }
      if (liveCodingInProgress(status) === false) break; // compile cleared
    }

    // Collect the stored result. poll:true never starts a new compile.
    return this.dispatch(LIVE_CODING_COMMAND, { poll: true, __lc_internal: true });
  }

  /**
   * One open→send→read→close round-trip. Resolves with the parsed envelope, or
   * rejects on a transport error / timeout so the caller can retry.
   */
  private sendCommandOnce(
    command: string,
    params: Record<string, unknown>,
  ): Promise<Envelope> {
    const payload = JSON.stringify({ type: command, params });
    const recvTimeout = recvTimeoutFor(command);

    return new Promise<Envelope>((resolve, reject) => {
      const chunks: Uint8Array[] = [];
      let settled = false;
      // GAP-003: once the payload is written, any later failure must NOT trigger a
      // retry (the command may be executing). Tracked so fail() can pick the error type.
      let payloadSent = false;
      let socket: Awaited<ReturnType<typeof Bun.connect>> | undefined;

      const decoder = new TextDecoder("utf-8", { fatal: true });
      // A recv timeout always means the payload was already sent → non-retryable.
      const overall = setTimeout(() => fail(`recv timeout after ${recvTimeout}ms`, true), recvTimeout);
      const connectTimer = setTimeout(
        () => fail(`connect timeout after ${CONNECT_TIMEOUT_MS}ms`),
        CONNECT_TIMEOUT_MS,
      );

      const cleanup = () => {
        clearTimeout(overall);
        clearTimeout(connectTimer);
        try {
          socket?.end();
        } catch {
          /* already closed */
        }
      };

      const done = (env: Envelope) => {
        if (settled) return;
        settled = true;
        cleanup();
        resolve(env);
      };

      const fail = (msg: string, postSend = false) => {
        if (settled) return;
        settled = true;
        cleanup();
        // A failure after the payload was sent is non-retryable regardless of which
        // callback reported it (recv timeout, socket error, or a post-send close).
        reject(postSend || payloadSent ? new PostSendError(msg) : new Error(msg));
      };

      /** Try to parse everything received so far as one JSON object. */
      const tryComplete = (): boolean => {
        if (chunks.length === 0) return false;
        const buf = concat(chunks);
        let text: string;
        try {
          text = decoder.decode(buf, { stream: false });
        } catch {
          return false; // partial UTF-8 — keep reading
        }
        try {
          const parsed = JSON.parse(text);
          done(normalizeEnvelope(parsed));
          return true;
        } catch {
          return false; // incomplete JSON — keep reading
        }
      };

      Bun.connect({
        hostname: this.host,
        port: this.port,
        socket: {
          open(s) {
            clearTimeout(connectTimer);
            s.write(payload);
            payloadSent = true;
          },
          data(_s, data) {
            chunks.push(data);
            tryComplete();
          },
          close() {
            // Remote closed. If we have a complete object, use it; else error.
            if (!settled && !tryComplete()) {
              fail("connection closed before a complete response");
            }
          },
          error(_s, err) {
            fail(err instanceof Error ? err.message : String(err));
          },
          connectError(_s, err) {
            fail(err instanceof Error ? err.message : String(err));
          },
        },
      })
        .then((s) => {
          socket = s;
        })
        .catch((err) => fail(err instanceof Error ? err.message : String(err)));
    });
  }

  // ── boot gate ──────────────────────────────────────────────────────

  /**
   * Hold until the editor is interactive, proves unreachable, or we exhaust the
   * patience budget. Never dispatches the real command.
   */
  private async awaitEditorReady(): Promise<"ready" | "engine_unavailable" | "timeout"> {
    if (this.editorReady) return "ready";

    const deadline = Date.now() + READINESS_TIMEOUT_MS;
    let delay = READINESS_POLL_BASE_MS;
    while (true) {
      let ready: boolean;
      try {
        ready = await this.probeReady();
      } catch {
        // Socket refused/timed out → editor isn't running (or the listener
        // hasn't bound yet). Surface the clean "unavailable" fast; do NOT
        // dispatch the real command.
        this.editorReady = false;
        return "engine_unavailable";
      }
      if (ready) {
        this.editorReady = true;
        return "ready";
      }
      if (Date.now() >= deadline) return "timeout";
      await sleep(Math.min(delay, Math.max(0, deadline - Date.now())));
      delay = Math.min(delay * 1.5, READINESS_POLL_MAX_MS);
    }
  }

  /** One `mcp_status` round-trip. Rejects on connection failure. */
  private async probeReady(): Promise<boolean> {
    const resp = await this.sendCommandOnce("mcp_status", {});
    const result = isRecord(resp) ? resp.result : undefined;
    return isRecord(result) && result.ready === true;
  }
}

// ── helpers ──────────────────────────────────────────────────────────

function backoff(attempt: number): number {
  return Math.min(BASE_RETRY_DELAY_MS * 2 ** attempt, MAX_RETRY_DELAY_MS);
}

function concat(chunks: Uint8Array[]): Uint8Array {
  if (chunks.length === 1) return chunks[0]!;
  const total = chunks.reduce((n, c) => n + c.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const c of chunks) {
    out.set(c, off);
    off += c.length;
  }
  return out;
}

/** Coerce a parsed bridge reply into our Envelope shape (defensive). */
function normalizeEnvelope(parsed: unknown): Envelope {
  if (isRecord(parsed) && (parsed.status === "success" || parsed.status === "error")) {
    return parsed as unknown as Envelope;
  }
  // A handler that returned a bare object without the wrapper — treat as success
  // payload to stay tolerant, matching the Python pass-through behaviour.
  return { status: "success", result: parsed };
}

/** Process-wide singleton, lazily created (mirrors `get_unreal_connection`). */
let singleton: UnrealConnection | undefined;
export function getConnection(): UnrealConnection {
  return (singleton ??= new UnrealConnection());
}
