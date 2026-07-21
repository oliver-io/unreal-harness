/**
 * Domain: pie — Play-In-Editor lifecycle, live-world queries, synthetic input.
 *
 * Port of the `pie_*` tools in `src/MCP/server.py`. These drive PIE (start/stop),
 * read the live PIE world (which the editor-world actor tools never see), and
 * inject synthetic keyboard/mouse input. None are blocked during PIE — they are
 * the automation surface for a running session — and none support dry_run.
 */

import { z } from "zod";
import type { ToolDef, ToolContext } from "../registry/types.ts";
import { bridgeTool, defineTool } from "./_shared.ts";
import { config } from "../config.ts";
import { type Envelope, isRecord } from "../bridge/envelope.ts";
import {
  acquire,
  release,
  markStarted,
  snapshot,
  type AcquireResult,
  type EvictReason,
  type PublicHolder,
} from "../pie/lease.ts";
import type { UnrealConnection } from "../bridge/connection.ts";
import { analyzeVideoFile } from "./video.ts";

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

// ── PIE lease coordination ────────────────────────────────────────────────
//
// One editor → one PIE world → one agent at a time. `pie_start`/`pie_stop` are
// wrapped by an in-process lease (src/pie/lease.ts) so concurrent agents take
// turns instead of stomping each other. The lease IDs agents by MCP sessionId;
// it is reset whenever the server restarts.
//
// `error_code`s below are SERVER-side coordination signals (not the C++ closed
// taxonomy): an agent that only checks envelope `status` correctly treats
// "wait"/"evicted" as a non-start; one that reads `result.pie_lease.state` gets
// the precise outcome.
const PIE_BUSY = "pie_busy"; // someone else holds the lease — wait & re-call
const PIE_LEASE_LOST = "pie_lease_lost"; // your lease expired and was reassigned
const PIE_NOT_HOLDER = "pie_not_holder"; // you never held the lease
const PIE_TAKEOVER_FAILED = "pie_takeover_failed"; // couldn't clear a stale PIE

/** The `result.pie_lease` block surfaced to agents on every lease decision. */
function leaseBlock(
  state: string,
  data: { acq?: AcquireResult; message: string; holder?: PublicHolder | null },
): Record<string, unknown> {
  const holder = data.acq ? data.acq.holder : (data.holder ?? null);
  return {
    state,
    message: data.message,
    you_hold: state === "started" || state === "already_holding",
    position: data.acq?.position ?? null,
    queue_length: data.acq?.queue_length ?? snapshot().queue_length,
    lease_ttl_seconds: Math.round(config.pieLeaseTtlMs / 1000),
    holder,
  };
}

/** Add a `pie_lease` block to a (bridge or synthetic) envelope's result. */
function withLease(env: Envelope, block: Record<string, unknown>): Envelope {
  const base = isRecord(env.result) ? env.result : {};
  return { ...env, result: { ...base, pie_lease: block } };
}

async function editorPieRunning(conn: UnrealConnection): Promise<boolean> {
  const env = await conn.sendCommand("pie_get_state", {});
  return isRecord(env.result) && env.result.is_running === true;
}

/**
 * Reconcile the editor before a freshly-promoted agent starts: if a PIE is still
 * live (a crashed/evicted prior holder, or one started outside the lease), STOP
 * it and wait until it has fully ended — we must never inherit a session already
 * in use — then start a clean PIE. Returns the bridge `pie_start` envelope, or an
 * error envelope if the stale session could not be cleared.
 */
async function takeoverAndStart(
  conn: UnrealConnection,
  mapPath: string,
  inViewport: boolean,
  signal: AbortSignal | undefined,
): Promise<Envelope> {
  if (await editorPieRunning(conn)) {
    await conn.sendCommand("pie_stop", {}); // best-effort; async end
    const deadline = Date.now() + config.pieTakeoverTimeoutMs;
    while (await editorPieRunning(conn)) {
      if (signal?.aborted) {
        return { status: "error", error: "pie_start aborted during takeover." };
      }
      if (Date.now() >= deadline) {
        return {
          status: "error",
          error_code: PIE_TAKEOVER_FAILED,
          error:
            `A previous PIE session did not stop within ` +
            `${Math.round(config.pieTakeoverTimeoutMs / 1000)}s; cannot start a ` +
            `clean session. The editor may need manual attention.`,
        };
      }
      await sleep(500);
    }
  }
  const params: Record<string, unknown> = {};
  if (mapPath) params.map_path = mapPath;
  if (inViewport) params.in_viewport = true;
  return conn.sendCommand("pie_start", params);
}

function sessionOf(ctx: ToolContext): string {
  return ctx.sessionId ?? "_anonymous";
}

/** Map pie_stop args → bridge params (omit empty session_id, matching Python). */
function forwardStopParams(args: { session_id: string }): Record<string, unknown> {
  return args.session_id ? { session_id: args.session_id } : {};
}

const pieStart = defineTool({
  name: "pie_start",
  domain: "pie",
  description:
    "Acquire the shared PIE lease and start a Play-In-Editor session. Only ONE " +
    "agent may run PIE at a time; this serializes access across agents. If the " +
    "lease is free you get it and PIE starts (returns immediately — poll " +
    "pie_get_state for readiness). If another agent holds it you get a 'wait' " +
    "response (status:error, error_code:pie_busy) with your queue position — " +
    "CALL pie_start AGAIN to keep your place (FIFO); you are promoted when the " +
    "holder calls pie_stop or its 10-minute lease expires. On a stale/crashed " +
    "prior session the lease stops it before starting yours. map_path optionally " +
    "loads a map first.",
  input: z.object({
    map_path: z
      .string()
      .default("")
      .describe(
        'Optional map asset path to load before starting PIE (e.g. "/Game/Maps/TestMap").',
      ),
    requester: z
      .string()
      .default("")
      .describe(
        "Optional human-readable label for the queue (e.g. a task name) — purely " +
          "diagnostic, shown to other agents in the lease/queue readout.",
      ),
    in_viewport: z
      .union([z.boolean(), z.string()]) // string-coerce: stale client schemas send "true"
      .default(false)
      .transform((v) => v === true || v === "true" || v === "1")
      .describe(
        "Run PIE inside the level-editor viewport instead of the Play-settings " +
          "window. Use when the editor viewport is being Pixel-Streamed (a remote " +
          "viewer can only see in-viewport PIE).",
      ),
  }),
  handler: async (args, ctx: ToolContext) => {
    const sessionId = sessionOf(ctx);
    const label = args.requester.trim() || `session ${sessionId.slice(0, 8)}`;
    const acq = await acquire(sessionId, label, ctx.signal, config.pieAcquireCapMs);

    if (acq.outcome === "aborted") {
      return withLease(
        { status: "error", error: "pie_start aborted (client disconnected or cancelled)." },
        leaseBlock("aborted", { acq, message: "Request aborted; queue slot released." }),
      );
    }

    if (acq.outcome === "waiting") {
      const who = acq.holder?.label ?? "another agent";
      const waitS = acq.holder ? Math.max(0, Math.round(acq.holder.expires_in_ms / 1000)) : 0;
      return withLease(
        {
          status: "error",
          error_code: PIE_BUSY,
          error:
            `PIE is in use by ${who}. You are queued at position ${acq.position}. ` +
            `This is NOT a failure to abort on — call pie_start again to hold your ` +
            `place. You will be promoted when the holder stops or its lease expires ` +
            `(in up to ~${waitS}s).`,
        },
        leaseBlock("waiting", {
          acq,
          message: "Re-call pie_start to keep your place in line (FIFO).",
        }),
      );
    }

    if (acq.outcome === "already_held") {
      return withLease(
        { status: "success", result: {} },
        leaseBlock("already_holding", {
          acq,
          message:
            "You already hold the PIE lease; not restarting. Poll pie_get_state " +
            "for readiness, or pie_stop to release it for the next agent. Lease TTL refreshed.",
        }),
      );
    }

    // Fresh acquire → reconcile the editor and start a clean PIE.
    const startEnv = await takeoverAndStart(ctx.conn, args.map_path, args.in_viewport, ctx.signal);
    if (startEnv.status === "error") {
      // Don't hold a lease we couldn't use — release so the next agent proceeds.
      release(sessionId);
      return withLease(
        startEnv,
        leaseBlock("start_failed", {
          message:
            "Could not start PIE; the lease was released. Inspect the error and retry pie_start.",
        }),
      );
    }
    // PIE is now starting for this holder — arm the keep-alive's liveness clock.
    markStarted(sessionId);
    return withLease(
      startEnv,
      leaseBlock("started", {
        acq,
        message:
          "PIE lease acquired and session starting. Poll pie_get_state for readiness; " +
          "call pie_stop when done to free the lease for the next agent.",
      }),
    );
  },
});

const pieStop = defineTool({
  name: "pie_stop",
  domain: "pie",
  description:
    "Release the shared PIE lease and stop your Play-In-Editor session, promoting " +
    "the next agent in line. Only the lease holder may stop PIE: if your lease " +
    "expired and was reassigned you get error_code:pie_lease_lost (and the current " +
    "session is left untouched); if you never held it, pie_not_holder. session_id " +
    "(from pie_start) is validated against the active session when provided.",
  input: z.object({
    session_id: z
      .string()
      .default("")
      .describe(
        "Optional engine session ID from pie_start; if provided, validates it " +
          "matches the active session before stopping.",
      ),
  }),
  annotations: { idempotentHint: true },
  handler: async (args, ctx: ToolContext) => {
    const sessionId = sessionOf(ctx);
    const rel = release(sessionId);

    if (rel.outcome === "evicted") {
      const why: Record<EvictReason, string> = {
        expired: "expired (held past the 10-minute limit while still running)",
        pie_ended: "was released because its PIE session stopped or crashed",
        pie_failed_start: "was released because PIE never started",
        editor_down: "was released because the editor became unreachable",
      };
      const reason = rel.reason ?? "expired";
      return withLease(
        {
          status: "error",
          error_code: PIE_LEASE_LOST,
          error:
            `Your PIE lease ${why[reason]}; you no longer hold it. The current ` +
            "editor state is not yours to stop — NOT forwarding pie_stop. Re-queue " +
            "with pie_start if you still need PIE.",
        },
        leaseBlock("lease_lost", {
          holder: rel.holder,
          message: `Lease lost (${reason}); ${
            rel.holder ? "another agent now holds it" : "the lease is now free"
          }.`,
        }),
      );
    }

    if (rel.outcome === "not_holder") {
      // Another agent holds the lease → refuse: never stop their live session.
      if (rel.holder) {
        return withLease(
          {
            status: "error",
            error_code: PIE_NOT_HOLDER,
            error:
              `You do not hold the PIE lease — it is held by ${rel.holder.label}. ` +
              "Not stopping their session. Use pie_start to queue for your own.",
          },
          leaseBlock("not_holder", {
            holder: rel.holder,
            message: "Acquire the lease with pie_start before stopping PIE.",
          }),
        );
      }
      // Lease is FREE: forward to the bridge so an untracked/orphaned PIE (e.g.
      // one left running across a server restart, which wipes the lease) can
      // still be cleaned up. The bridge returns not_in_pie if nothing is running.
      const orphanEnv = await ctx.conn.sendCommand("pie_stop", forwardStopParams(args));
      return withLease(
        orphanEnv,
        leaseBlock("not_holder", {
          message:
            "Lease was free; forwarded the stop to the editor to clear any " +
            "untracked PIE session.",
        }),
      );
    }

    // Holder → forward the stop to the bridge.
    const stopEnv = await ctx.conn.sendCommand("pie_stop", forwardStopParams(args));
    return withLease(
      stopEnv,
      leaseBlock("released", {
        message: "PIE lease released; the next queued agent (if any) may now start.",
      }),
    );
  },
});

const pieGetState = defineTool({
  name: "pie_get_state",
  domain: "pie",
  description:
    "Check if a Play-In-Editor session is running and get world info, PLUS the " +
    "shared lease state: who holds PIE, the FIFO wait queue, and whether YOU hold " +
    "it (result.pie_lease). Read-only — does not affect the lease.",
  input: z.object({
    random_string: z
      .string()
      .default("")
      .describe("Unused placeholder (no-arg tool)."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  handler: async (_args, ctx: ToolContext) => {
    const env = await ctx.conn.sendCommand("pie_get_state", {});
    return withLease(env, { ...snapshot(sessionOf(ctx)) });
  },
});

const pieQuery = bridgeTool({
  name: "pie_query",
  domain: "pie",
  description:
    "Query the LIVE Play-In-Editor world state — fills the gap left by the " +
    "editor-world actor tools (which never see PIE-spawned actors). Requires PIE " +
    "running. query: 'summary' (default), 'players'/'player'/'pawn' (piloted pawn " +
    "name + camera POV), 'actors' (filtered, capped by limit), or 'all'.",
  input: z.object({
    query: z
      .string()
      .default("summary")
      .describe(
        "What to return: 'summary' (world/map, totals, possessed_pawns), " +
          "'players'/'player'/'pawn' (per-controller pawn + velocity/view_target/camera_pov), " +
          "'actors' (substring-filtered PIE-world actor list, capped by limit), or 'all'.",
      ),
    filter: z
      .string()
      .default("")
      .describe(
        "Substring filter for query='actors' (matches name/label/class).",
      ),
    limit: z
      .number()
      .int()
      .default(200)
      .describe("Max actors returned for query='actors' (default 200)."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  // Always send query + limit; omit filter when empty (matches Python).
  params: (a) => {
    const p: Record<string, unknown> = { query: a.query, limit: a.limit };
    if (a.filter) p.filter = a.filter;
    return p;
  },
});

/** {"shift","ctrl","alt","cmd"} — keystroke modifier chord. */
const KeyModifiers = z.object({
  shift: z.boolean().optional(),
  ctrl: z.boolean().optional(),
  alt: z.boolean().optional(),
  cmd: z.boolean().optional(),
});

const Keystroke = z.object({
  key: z
    .string()
    .describe('UE key name (e.g. "I", "W", "SpaceBar", "Escape", "LeftShift").'),
  event_type: z.enum(["pressed", "released"]).describe('"pressed" or "released".'),
  delay_ms: z
    .number()
    .int()
    .optional()
    .describe("Milliseconds to wait BEFORE this event (default 0)."),
  modifiers: KeyModifiers.optional().describe(
    '{"shift","ctrl","alt","cmd"} booleans.',
  ),
});

const pieSendKeystrokesInput = z.object({
  actions: z
    .array(Keystroke)
    .describe(
      "Array of keystroke events executed in order. Each: key (required), " +
        "event_type ('pressed'|'released', required), delay_ms (optional, waits " +
        "before the event), modifiers (optional shift/ctrl/alt/cmd).",
    ),
  focus_viewport: z
    .boolean()
    .optional()
    .default(false)
    .describe(
      "Opt-in (default false). When true, focuses the PIE game viewport before " +
        "injecting each event so a HELD key reaches polled input (a pawn calling " +
        "IsInputKeyDown each frame). Without it, the editor — not the PIE viewport — " +
        "holds Slate keyboard focus, so the engine flushes the simulated press " +
        "(FlushPressedKeys on viewport-focus-loss) before the pawn polls it. Enabling " +
        "this STEALS keyboard focus from the editor window, so leave it off unless you " +
        "specifically need sustained polled input; no-op outside PIE.",
    ),
});

// Fans out one `pie_send_keystrokes` wire command per event, honoring per-event
// delay_ms server-side (matches the Python loop), and returns the aggregate.
const pieSendKeystrokes = defineTool<typeof pieSendKeystrokesInput>({
  name: "pie_send_keystrokes",
  domain: "pie",
  description:
    "Send one or more keyboard input events with optional delays. Each event " +
    "(key + 'pressed'/'released', optional delay_ms/modifiers) is sent in order; " +
    "delays are handled server-side so you get one response when done. " +
    'Example single tap: [{"key":"I","event_type":"pressed"},{"key":"I","event_type":"released","delay_ms":50}].',
  input: pieSendKeystrokesInput,
  handler: async (args, ctx: ToolContext) => {
    const events = args.actions;
    const results: unknown[] = [];
    for (const event of events) {
      const delayMs = event.delay_ms ?? 0;
      if (delayMs > 0) await sleep(delayMs);
      const params: Record<string, unknown> = {
        key: event.key,
        event_type: event.event_type,
        modifiers: event.modifiers ?? {},
      };
      if (args.focus_viewport) params.focus_viewport = true;
      results.push(await ctx.conn.sendCommand("pie_send_keystrokes", params));
    }
    return { success: true, events_sent: results.length, results };
  },
});

const pieSendMouse = bridgeTool({
  name: "pie_send_mouse",
  domain: "pie",
  description:
    "Send a mouse input event (move, click, release) at screen (x,y). " +
    "event_type: 'move' (default), 'pressed', or 'released'; button: 'left' " +
    "(default), 'right', or 'middle' (for press/release).",
  input: z.object({
    x: z.number().describe("Screen X coordinate."),
    y: z.number().describe("Screen Y coordinate."),
    event_type: z
      .string()
      .default("move")
      .describe('"move", "pressed", or "released".'),
    button: z
      .string()
      .default("left")
      .describe('"left", "right", or "middle" (for press/release events).'),
  }),
  // All four keys always sent (matches Python).
  params: (a) => ({
    x: a.x,
    y: a.y,
    event_type: a.event_type,
    button: a.button,
  }),
});

// Reproduce a camera pose inside the LIVE PIE world and screenshot it. The
// deterministic, no-navigation capture rig: pair with editor_viewport_get_camera to
// turn a human-framed editor shot into a reproducible in-game screenshot. This is the
// sanctioned way to validate a fixed view in-game (docs/TESTING.md / CLAUDE.md "rig first").
const PoseVec3 = z.object({
  x: z.number(),
  y: z.number(),
  z: z.number(),
});
const PoseRotator = z.object({
  pitch: z.number(),
  yaw: z.number(),
  roll: z.number(),
});

const pieCaptureFromPose = bridgeTool({
  name: "pie_capture_from_pose",
  domain: "pie",
  description:
    "Capture a screenshot from an EXACT camera pose inside the running game (PIE), through the " +
    "real game render path — no character or camera navigation. Spawns a transient camera at the " +
    "pose, swaps the player's view target to it (instant), waits a few frames for it to composite, " +
    "captures the game viewport, then (by default) restores the original view and destroys the temp " +
    "camera. Requires PIE running. Feed it location/rotation/fov/aspect from editor_viewport_get_camera " +
    "to reproduce a human-framed editor shot in-game. Returns result.path (status 'captured' once the " +
    "bridge confirms the file). This is the sanctioned way to get a reproducible in-game screenshot of " +
    "a fixed view for visual validation — never hand-fly the PIE camera to a spot and screenshot.",
  input: z.object({
    location: PoseVec3.describe(
      "Camera world location {x,y,z} — typically straight from editor_viewport_get_camera.",
    ),
    rotation: PoseRotator.describe("Camera rotation {pitch,yaw,roll}."),
    fov: z
      .number()
      .optional()
      .describe(
        "Horizontal field of view in degrees. Omit to use the camera default (90). Pass the editor " +
          "viewport's fov to match framing.",
      ),
    aspect: z
      .number()
      .optional()
      .describe(
        "If set (>0), constrain the captured view to this aspect ratio (letterboxed) so framing matches " +
          "the source viewport exactly. Omit to fill the game window's native aspect.",
      ),
    restore: z
      .boolean()
      .optional()
      .default(true)
      .describe(
        "After capture, restore the player's original view target and destroy the temp camera (default " +
          "true). Set false to leave the view parked at the pose for repeated captures.",
      ),
    filename: z
      .string()
      .optional()
      .describe("Output filename. Default: timestamped MCP_PoseCapture_YYYYMMDD_HHMMSS.png."),
    directory: z
      .string()
      .optional()
      .describe("Output directory. Default: the project Screenshots folder."),
  }),
  annotations: { readOnlyHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {
      location: a.location,
      rotation: a.rotation,
      restore: a.restore,
    };
    if (a.fov !== undefined) p.fov = a.fov;
    if (a.aspect !== undefined) p.aspect = a.aspect;
    if (a.filename) p.filename = a.filename;
    if (a.directory) p.directory = a.directory;
    return p;
  },
});

// ── PIE video recording (capture primitive) ─────────────────────────────
//
// Records the live PIE viewport (UMG/HUD composited in) to an H.264 MP4 in the
// engine — no external binary. Lease-aware: you may record YOUR session (you
// hold the PIE lease) or an unleased one (e.g. a human started PIE by hand);
// you may never record another agent's session.

/** Refuse when ANOTHER session holds the PIE lease; null = allowed. */
function recordLeaseGuard(ctx: ToolContext, verb: string): Envelope | null {
  const snap = snapshot(sessionOf(ctx));
  if (snap.holder && !snap.you_hold) {
    return withLease(
      {
        status: "error",
        error_code: PIE_NOT_HOLDER,
        error:
          `The PIE session belongs to ${snap.holder.label} (they hold the PIE ` +
          `lease) — not ${verb} someone else's session. Acquire PIE with ` +
          `pie_start first.`,
      },
      leaseBlock("not_holder", {
        holder: snap.holder,
        message: "Recording is limited to your own (or an unleased) PIE session.",
      }),
    );
  }
  return null;
}

const pieRecordStart = defineTool({
  name: "pie_record_start",
  domain: "pie",
  description:
    "Start recording the live PIE viewport to an H.264 MP4 (in-engine encode — " +
    "3D scene + UMG/HUD as presented, cropped to the game viewport and downscaled " +
    "to fit width/height), WITH the game audio the player hears (the PIE world's " +
    "main-submix mix, AAC 192kbps, sample-clock-synced to the video timeline; " +
    "audio:false or a missing audio device degrades to video-only — see " +
    "result.audio/audio_note). Requires PIE running and a real RHI (refused with " +
    "feature_disabled under -nullrhi: no frames are rendered there). One recording " +
    "at a time; a hard max_duration_s watchdog auto-stops and finalizes the file " +
    "so a recording can never leak. Lease-aware: refused if another agent holds " +
    "the PIE lease. Returns recording_id + the output path; stop with " +
    "pie_record_stop to finalize and get metadata.",
  input: z.object({
    fps: z
      .number()
      .default(30)
      .describe(
        "Capture frame rate. 0 = every presented frame (true live rate — use for " +
          "fast/transient behaviour); default 30.",
      ),
    width: z
      .number()
      .int()
      .default(1280)
      .describe("Max output width; the viewport is downscaled to FIT width x height, preserving aspect."),
    height: z.number().int().default(720).describe("Max output height (default 720)."),
    bitrate_kbps: z
      .number()
      .int()
      .default(8000)
      .describe("H.264 target bitrate in kbps (default 8000 = crisp 720p30)."),
    audio: z
      .boolean()
      .default(true)
      .describe(
        "Capture the PIE world's game audio (main-submix mix — exactly what the " +
          "player hears) as a synced AAC track (default true). Set false for video-only.",
      ),
    max_duration_s: z
      .number()
      .default(120)
      .describe(
        "Hard watchdog: the recording auto-stops and finalizes after this many " +
          "seconds (1-600, default 120). Not the intended length — call " +
          "pie_record_stop when done.",
      ),
    filename: z
      .string()
      .default("")
      .describe("Output filename (.mp4 appended). Default: timestamped MCP_Recording_*.mp4."),
    directory: z
      .string()
      .default("")
      .describe("Output directory. Default: <Project>/Saved/MCPRecordings/."),
    wait_for_pie_s: z
      .number()
      .min(0)
      .max(120)
      .default(0)
      .describe(
        "Arm-and-wait: keep retrying for up to this many seconds until the PIE " +
          "world is up and presenting, then start recording within ~0.5s of the " +
          "first playable frame. Call pie_start and THIS immediately after (no " +
          "manual sleep) so the opening seconds of play are never missed. 0 = " +
          "fail fast if PIE is not ready.",
      ),
  }),
  handler: async (args, ctx: ToolContext) => {
    const refusal = recordLeaseGuard(ctx, "starting a recording of");
    if (refusal) return refusal;
    const p: Record<string, unknown> = {
      fps: args.fps,
      width: args.width,
      height: args.height,
      bitrate_kbps: args.bitrate_kbps,
      audio: args.audio,
      max_duration_s: args.max_duration_s,
    };
    if (args.filename) p.filename = args.filename;
    if (args.directory) p.directory = args.directory;

    // Errors that mean "PIE isn't presentable YET" — retriable while arming.
    // Anything else (feature_disabled, engine_busy from an active recording,
    // invalid_argument) is a real refusal and returns immediately.
    const armDeadline = Date.now() + args.wait_for_pie_s * 1000;
    for (;;) {
      const env = await ctx.conn.sendCommand("pie_record_start", p);
      if (env.status === "success") return env;
      const retriable =
        env.error_code === "not_in_pie" ||
        (env.error_code === "engine_busy" && /zero size/i.test(env.error ?? "")) ||
        (env.error_code === "internal" && /PIE scene viewport/i.test(env.error ?? ""));
      if (!retriable || Date.now() >= armDeadline || ctx.signal?.aborted) return env;
      await sleep(500);
    }
  },
});

const pieRecordArm = defineTool({
  name: "pie_record_arm",
  domain: "pie",
  description:
    "ARM the recorder: from now until pie_record_disarm, EVERY PIE session on " +
    "this editor records itself automatically — capture starts the moment the " +
    "PIE world is up (no arming latency, the opening seconds are captured) and " +
    "finalizes when PIE ends or max_duration_s elapses. Takes are auto-numbered " +
    "<base_name>_NN.mp4 in the output directory. If PIE is already running, " +
    "recording starts immediately. NOTE: applies to any PIE session on this " +
    "editor, whether started by you, a human in the editor, or another agent — " +
    "arm only with the operator's knowledge. Re-arming updates the settings.",
  input: z.object({
    base_name: z
      .string()
      .default("MCP_Take")
      .describe('Take-name prefix; files land as "<base_name>_01.mp4", "_02", ….'),
    fps: z.number().default(30).describe("Capture frame rate; 0 = every presented frame."),
    width: z.number().int().default(1280).describe("Max output width (fit, aspect preserved)."),
    height: z.number().int().default(720).describe("Max output height."),
    bitrate_kbps: z.number().int().default(8000).describe("H.264 bitrate in kbps."),
    audio: z
      .boolean()
      .default(true)
      .describe("Capture the player's game audio as a synced AAC track (default true)."),
    max_duration_s: z
      .number()
      .default(120)
      .describe("Per-take hard cap (1-600 s): the take finalizes past this even if PIE keeps running."),
    directory: z
      .string()
      .default("")
      .describe("Output directory. Default: <Project>/Saved/MCPRecordings/."),
  }),
  handler: async (args, ctx: ToolContext) => {
    const refusal = recordLeaseGuard(ctx, "arming auto-recording of");
    if (refusal) return refusal;
    const p: Record<string, unknown> = {
      base_name: args.base_name,
      fps: args.fps,
      width: args.width,
      height: args.height,
      bitrate_kbps: args.bitrate_kbps,
      audio: args.audio,
      max_duration_s: args.max_duration_s,
    };
    if (args.directory) p.directory = args.directory;
    return ctx.conn.sendCommand("pie_record_arm", p);
  },
});

const pieRecordDisarm = defineTool({
  name: "pie_record_disarm",
  domain: "pie",
  description:
    "Stop auto-recording future PIE sessions (undoes pie_record_arm). A take " +
    "that is currently recording keeps going — it still finalizes on PIE end, " +
    "the per-take cap, or pie_record_stop. Returns how many takes were recorded " +
    "while armed.",
  input: z.object({
    random_string: z.string().default("").describe("Unused placeholder (no-arg tool)."),
  }),
  annotations: { idempotentHint: true },
  handler: async (_args, ctx: ToolContext) => {
    const refusal = recordLeaseGuard(ctx, "disarming auto-recording of");
    if (refusal) return refusal;
    return ctx.conn.sendCommand("pie_record_disarm", {});
  },
});

const pieRecordStop = defineTool({
  name: "pie_record_stop",
  domain: "pie",
  description:
    "Stop the active PIE recording and finalize the MP4. Returns path, " +
    "width/height, frames_encoded, frames_dropped, duration_s, bytes, and " +
    "stop_reason. Recordings also auto-finalize on PIE end and on the " +
    "max_duration_s watchdog — a stop after that returns 'no recording in progress'.",
  input: z.object({
    random_string: z.string().default("").describe("Unused placeholder (no-arg tool)."),
  }),
  handler: async (_args, ctx: ToolContext) => {
    const refusal = recordLeaseGuard(ctx, "stopping a recording of");
    if (refusal) return refusal;
    return ctx.conn.sendCommand("pie_record_stop", {});
  },
});

const pieRecordStatus = bridgeTool({
  name: "pie_record_status",
  domain: "pie",
  description:
    "Check PIE recording state: whether a recording is active, its recording_id, " +
    "output path, elapsed_s, frames captured/encoded/dropped, and any encoder " +
    "error. Read-only.",
  input: z.object({
    random_string: z.string().default("").describe("Unused placeholder (no-arg tool)."),
  }),
  annotations: { readOnlyHint: true, idempotentHint: true },
  params: () => ({}),
});

/** Sleep that resolves early (without throwing) when the request aborts. */
function abortableSleep(ms: number, signal: AbortSignal | undefined): Promise<void> {
  return new Promise<void>((resolve) => {
    const timer = setTimeout(done, ms);
    function done() {
      clearTimeout(timer);
      signal?.removeEventListener("abort", done);
      resolve();
    }
    signal?.addEventListener("abort", done, { once: true });
  });
}

// The ergonomic one-shot: record N seconds of the live PIE session, then judge
// the clip against a stated expectation. Composite in the server layer per the
// harness doctrine — the C++ only ever sees pie_record_start/stop.
const pieAnalyze = defineTool({
  name: "pie_analyze",
  domain: "pie",
  description:
    "Record the live PIE session for duration_s and judge the clip against a " +
    "stated expected behaviour with a video-understanding model — the one-shot " +
    "composite over pie_record_start → wait → pie_record_stop → video_analyze. " +
    "Returns the recording metadata (path stays on disk) plus the structured " +
    "analysis (verdict, timestamped divergences, per-criterion results). Drive " +
    "the scenario yourself (pie_send_keystrokes / pie_inject_input_action) while " +
    "it records — this call occupies the session for its whole duration. " +
    "Requires PIE running, a real RHI, and GEMINI_API_KEY. Expect duration_s + " +
    "tens of seconds of latency (upload + inference); for anything intricate " +
    "prefer the explicit pie_record_* + video_analyze sequence.",
  input: z.object({
    expected_behavior: z
      .string()
      .describe("What SHOULD happen during the recorded window, in plain language."),
    criteria: z
      .array(z.string())
      .default([])
      .describe("Optional explicit checks, judged individually."),
    duration_s: z
      .number()
      .min(1)
      .max(120)
      .default(10)
      .describe("How long to record before analyzing (1-120 s, default 10)."),
    capture_fps: z
      .number()
      .default(30)
      .describe("Recorder frame rate; 0 = every presented frame (default 30)."),
    width: z.number().int().default(1280).describe("Max output width (fit, aspect preserved)."),
    height: z.number().int().default(720).describe("Max output height."),
    analysis_fps: z
      .number()
      .default(0)
      .describe("Model sampling fps (default: server config, normally 1). ~300 tokens/sampled-second."),
    model: z.string().default("").describe("Video model id override."),
  }),
  handler: async (args, ctx: ToolContext) => {
    const refusal = recordLeaseGuard(ctx, "recording/analyzing");
    if (refusal) return refusal;

    const startEnv = await ctx.conn.sendCommand("pie_record_start", {
      fps: args.capture_fps,
      width: args.width,
      height: args.height,
      // Bound the recording just past the requested window so a dropped
      // connection can never leave it running unattended.
      max_duration_s: Math.min(600, args.duration_s + 30),
    });
    if (startEnv.status !== "success") return startEnv;

    await abortableSleep(args.duration_s * 1000, ctx.signal);

    const stopEnv = await ctx.conn.sendCommand("pie_record_stop", {});
    if (stopEnv.status !== "success" || !isRecord(stopEnv.result)) return stopEnv;
    const path = typeof stopEnv.result.path === "string" ? stopEnv.result.path : "";
    if (!path) {
      return { status: "error", error: "pie_record_stop returned no file path.", result: stopEnv.result };
    }

    const analysis = await analyzeVideoFile({
      path,
      expected_behavior: args.expected_behavior,
      criteria: args.criteria,
      analysis_fps: args.analysis_fps,
      model: args.model,
    });
    return {
      status: analysis.status ?? "success",
      ...(typeof analysis.error === "string" ? { error: analysis.error } : {}),
      result: { recording: stopEnv.result, analysis: analysis.result ?? null },
    };
  },
});

const pieInjectInputAction = bridgeTool({
  name: "pie_inject_input_action",
  domain: "pie",
  description:
    "Inject an Enhanced Input action into the live PIE player (a one-shot press). " +
    "This is the typed way to trigger event-driven actions — jump, traversal/vault, " +
    "dash — that synthetic keystrokes (pie_send_keystrokes) can't fire (only " +
    "continuous/axis input survives key simulation). Pass the /Game/... path of the " +
    "UInputAction (IA_*) and an optional value (default 1.0 = pressed).",
  input: z.object({
    action_path: z
      .string()
      .describe("/Game/... path of the UInputAction asset (e.g. an IA_* action)."),
    value: z
      .number()
      .default(1.0)
      .describe("Raw action value to inject (default 1.0; non-zero = pressed)."),
  }),
  params: (a) => ({ action_path: a.action_path, value: a.value }),
});

export const pieTools: ToolDef[] = [
  pieStart,
  pieStop,
  pieGetState,
  pieQuery,
  pieSendKeystrokes,
  pieSendMouse,
  pieCaptureFromPose,
  pieInjectInputAction,
  pieRecordStart,
  pieRecordArm,
  pieRecordDisarm,
  pieRecordStop,
  pieRecordStatus,
  pieAnalyze,
];
