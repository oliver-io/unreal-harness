/**
 * Runtime configuration. Env-overridable with sane localhost defaults so the
 * server "just works" with `bun run mcp` and no setup.
 */

import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";

/**
 * Secrets live in the gitignored REPO-ROOT .env by convention, but Bun only
 * auto-loads ./.env relative to the cwd (src/server when launched via
 * scripts/run-server). Backfill any unset vars from the root file so keys work
 * regardless of where the server was started from. Never overrides real env.
 */
function loadRootEnvFallback(): void {
  const path = join(import.meta.dir, "..", "..", "..", ".env");
  if (!existsSync(path)) return;
  for (const rawLine of readFileSync(path, "utf8").split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith("#")) continue;
    const eq = line.indexOf("=");
    if (eq <= 0) continue;
    const key = line.slice(0, eq).trim();
    let value = line.slice(eq + 1).trim();
    if (
      (value.startsWith('"') && value.endsWith('"')) ||
      (value.startsWith("'") && value.endsWith("'"))
    ) {
      value = value.slice(1, -1);
    }
    if (process.env[key] === undefined) process.env[key] = value;
  }
}
loadRootEnvFallback();

const num = (v: string | undefined, fallback: number): number => {
  const n = v === undefined ? NaN : Number(v);
  return Number.isFinite(n) ? n : fallback;
};

export const config = {
  /** MCP streamable-HTTP listener (the client talks to us here). */
  mcpHost: process.env.UNREAL_MCP_HOST ?? "127.0.0.1",
  mcpPort: num(process.env.UNREAL_MCP_PORT, 8765),

  /** Unreal editor C++ bridge (we forward JSON commands here over raw TCP). */
  bridgeHost: process.env.UNREAL_BRIDGE_HOST ?? "127.0.0.1",
  bridgePort: num(process.env.UNREAL_BRIDGE_PORT, 55557),

  logLevel: (process.env.UNREAL_MCP_LOG_LEVEL ?? "info") as
    | "debug"
    | "info"
    | "warn"
    | "error",

  /**
   * Which tool surface to advertise (progressive disclosure):
   *   full    — all 233 domain tools + catalog_* meta-tools (direct access +
   *             discovery). Default; best for clients with native tool-search.
   *   compact — core + catalog_* only; the 233 are reached via catalog_call.
   *             Minimal schema tax for clients without tool-search.
   *   code    — core + catalog_* + code_api/code_run (execute TS that drives the
   *             editor; intermediate data stays out of context). Opt-in.
   */
  surface: (process.env.UNREAL_MCP_SURFACE ?? "full") as "full" | "compact" | "code",

  /**
   * Result-compaction threshold in bytes. When > 0, a tool result whose JSON
   * exceeds this is replaced with a digest + handle (read back via result_read).
   * Default 0 (OFF) → direct results stay byte-identical to the Python server.
   */
  maxResultBytes: num(process.env.UNREAL_MCP_MAX_RESULT_BYTES, 0),

  /**
   * PIE lease coordination (multiple agents sharing one editor). In-memory only —
   * a server restart resets the whole queue and everyone re-queues.
   *   ttl      — how long a holder keeps the lease before it is considered stuck
   *              and reaped so the next agent in line is promoted (10 min default).
   *   acquire  — how long a single pie_start call long-polls for the lease before
   *              returning a `waiting` response (the agent re-calls to keep its
   *              place). Keep under the client's request timeout (~25s default).
   *   takeover — how long to wait for a stale PIE to actually end after we issue
   *              the stop, before the promoted agent starts its own session.
   *
   * Keep-alive (the lease reconciler watches real editor PIE state so a crashed
   * session frees the lease early, instead of stalling everyone until the TTL):
   *   poll        — how often to probe editor PIE state while the lease is held.
   *   startup     — grace after pie_start before "not running" counts as a failed
   *                 start (covers map load + the few-frame PIE warm-up).
   *   editorDown  — grace of continuous editor unreachability before the holder's
   *                 lease is released (rides out transient blips / brief restarts).
   */
  pieLeaseTtlMs: num(process.env.UNREAL_MCP_PIE_LEASE_TTL_MS, 10 * 60 * 1000),
  pieAcquireCapMs: num(process.env.UNREAL_MCP_PIE_ACQUIRE_CAP_MS, 25_000),
  pieTakeoverTimeoutMs: num(process.env.UNREAL_MCP_PIE_TAKEOVER_TIMEOUT_MS, 30_000),
  pieLivenessPollMs: num(process.env.UNREAL_MCP_PIE_LIVENESS_POLL_MS, 5_000),
  pieStartupGraceMs: num(process.env.UNREAL_MCP_PIE_STARTUP_GRACE_MS, 60_000),
  pieEditorDownGraceMs: num(process.env.UNREAL_MCP_PIE_EDITOR_DOWN_GRACE_MS, 30_000),

  /**
   * Build lock — serializes C++ builds (driven by the build scripts over the
   * /build REST endpoints) so two agents don't rebuild at once and confuse each
   * other when the editor goes down. In-memory: a server restart clears it.
   * A crashed build frees the lock via PID liveness almost immediately; the TTL
   * is the backstop for a build that hangs while its process stays alive (a full
   * editor recompile can run tens of minutes, so this is generous).
   */
  buildLockTtlMs: num(process.env.UNREAL_MCP_BUILD_LOCK_TTL_MS, 45 * 60 * 1000),

  /**
   * Video analysis (video_analyze / pie_analyze) — the one vendor-coupled layer
   * of the PIE video pipeline, quarantined behind src/video/analyzer.ts. The
   * recorded MP4 itself is produced in-engine and is provider-agnostic.
   *   provider     — analysis backend ("google" is the only one implemented).
   *   model        — a Gemini video-understanding model id.
   *   analysisFps  — how densely the MODEL samples the upload (≈300 tokens per
   *                  sampled second — sample as low as the question allows).
   *                  Distinct from the recorder's capture fps (source fidelity).
   *   key          — GEMINI_API_KEY preferred; GOOGLE_STUDIO_API_KEY accepted.
   */
  videoProvider: process.env.UNREAL_MCP_VIDEO_PROVIDER ?? "google",
  videoModel: process.env.UNREAL_MCP_VIDEO_MODEL ?? "gemini-3.5-flash",
  videoDefaultAnalysisFps: num(process.env.UNREAL_MCP_VIDEO_ANALYSIS_FPS, 1),
  videoMaxAnalysisFps: num(process.env.UNREAL_MCP_VIDEO_MAX_ANALYSIS_FPS, 30),
  videoUploadTimeoutMs: num(process.env.UNREAL_MCP_VIDEO_UPLOAD_TIMEOUT_MS, 120_000),
  geminiApiKey: process.env.GEMINI_API_KEY ?? process.env.GOOGLE_STUDIO_API_KEY ?? "",
} as const;
