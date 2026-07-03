/**
 * Domain: editor — editor-process operations: screenshots, console exec,
 * asset-registry refresh, perf snapshot, live-coding/UBT compiles, unified log.
 *
 * Port of the `editor_*` tools in `src/MCP/server.py`. Most forward to the
 * bridge; `editor_build_game_target` and `editor_read_logs` run entirely on the
 * host (subprocess / filesystem) and never touch the bridge — they reproduce
 * the Python helpers (`_resolve_project_root`, `_unified_log_path`,
 * `_read_log_lines`, `_parse_seq`) inline.
 */

import { spawnSync } from "node:child_process";
import { readdirSync, readFileSync, statSync } from "node:fs";
import { basename, join } from "node:path";
import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool, defineTool } from "./_shared.ts";
import { log } from "../log.ts";

// One-shot frame/memory/GPU snapshot. Read-only; values are instantaneous.
const editorPerfSnapshot = bridgeTool({
  name: "editor_perf_snapshot",
  domain: "editor",
  description:
    "One-shot snapshot of editor frame stats and memory usage. Snapshot, not a " +
    "profile — average_fps / average_frame_ms reflect the engine's running " +
    "averages at the instant of the call. Returns frame stats, memory{}, gpu{}.",
  input: z.object({}),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

// Synchronous asset-registry rescan of a content path. 1:1 params.
const editorContentBrowserRefresh = bridgeTool({
  name: "editor_content_browser_refresh",
  domain: "editor",
  description:
    "Force a synchronous asset-registry rescan of a content path. Useful after " +
    "external filesystem changes (marketplace drop-ins, VCS checkouts) when the " +
    "in-process registry cache is stale. Routes through ScanPathsSynchronous.",
  input: z.object({
    path: z
      .string()
      .default("/Game")
      .describe("Logical content path. Default /Game. Plugin paths like /Engine valid."),
    force_rescan: z
      .boolean()
      .default(true)
      .describe("When true (default), rescans even if previously seen."),
  }),
  annotations: { idempotentHint: true },
});

// Capture an editor window / its viewport. Omit-when-unset params.
const editorWindowScreenshot = bridgeTool({
  name: "editor_window_screenshot",
  domain: "editor",
  description:
    "Capture a PNG of an editor window or its level viewport. Target priority: " +
    "viewport=true (active level viewport) > tab_name (window containing that " +
    "dock tab) > window_title (top-level window, case-insensitive substring) > " +
    "no args (editor root window). Saves to Saved/MCPScreenshots/.",
  input: z.object({
    viewport: z
      .boolean()
      .default(false)
      .describe("True to capture just the active SLevelViewport (3D scene + gizmos)."),
    tab_name: z
      .string()
      .default("")
      .describe('Registered tab id (FName), e.g. "LevelEditor", "OutputLog". Exact, case-sensitive.'),
    window_title: z
      .string()
      .default("")
      .describe("Case-insensitive substring of a top-level SWindow title (undocked windows)."),
  }),
  annotations: { readOnlyHint: true },
  // Matches Python: only forward each arg when set/truthy.
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.viewport) p.viewport = true;
    if (a.tab_name) p.tab_name = a.tab_name;
    if (a.window_title) p.window_title = a.window_title;
    return p;
  },
});

// Game-viewport or full-editor screenshot. Omit empty filename/directory.
const editorScreenshot = bridgeTool({
  name: "editor_screenshot",
  domain: "editor",
  description:
    'Take a screenshot. mode="viewport" (default) captures the game viewport only; ' +
    'mode="editor" captures the full editor window (panels, menus, toolbar, viewport). ' +
    "Returns the file path where the image is saved (result.path). " +
    "The bridge forces the editor viewport to render and CONFIRMS the output file exists " +
    "before returning — result.status is \"captured\" on success. If the editor window is " +
    "occluded, minimized, or backgrounded (so it never composites a frame), the capture is " +
    "partial by nature: instead of falsely reporting success it returns a `timeout` error. " +
    "Foreground the editor (and turn off 'Use Less CPU when in Background') for reliable " +
    'capture, or use mode="editor" (synchronous OS-level grab) when the window is visible.',
  input: z.object({
    filename: z
      .string()
      .default("")
      .describe("Optional filename. Default: timestamped MCP_Screenshot_YYYYMMDD_HHMMSS.png"),
    directory: z
      .string()
      .default("")
      .describe("Optional output directory. Default: project Screenshots folder."),
    mode: z
      .string()
      .default("viewport")
      .describe('"viewport" (default, game viewport only) or "editor" (full editor window).'),
  }),
  annotations: { readOnlyHint: true },
  params: (a) => {
    const p: Record<string, unknown> = {};
    if (a.filename) p.filename = a.filename;
    if (a.directory) p.directory = a.directory;
    if (a.mode) p.mode = a.mode;
    return p;
  },
});

// Read the active editor perspective viewport camera pose. The "record my framing"
// half of the capture rig: a human frames a shot in the level viewport, this captures
// it so pie_capture_from_pose can reproduce the exact same view inside the running game.
const editorViewportGetCamera = bridgeTool({
  name: "editor_viewport_get_camera",
  domain: "editor",
  description:
    "Read the active level-editor PERSPECTIVE viewport camera pose: " +
    "{location{x,y,z}, rotation{pitch,yaw,roll}, fov, aspect, ortho, viewport_size{x,y}}. " +
    "Prefers the last-focused viewport (the one you just framed), falling back to the first " +
    "perspective viewport. Use this to RECORD a human-framed shot, then reproduce it inside the " +
    "running game with pie_capture_from_pose (pass the same location/rotation/fov/aspect). " +
    "If result.ortho is true the viewport is orthographic and fov is not meaningful — frame a " +
    "perspective viewport instead.",
  input: z.object({}),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

// Run a console command in the PIE world (if running) or the editor world.
const editorConsoleExec = bridgeTool({
  name: "editor_console_exec",
  domain: "editor",
  description:
    "Execute a console command in the PIE world (if running) or the editor world. " +
    'E.g. "stat fps", "show collision".',
  input: z.object({
    command: z
      .string()
      .describe('Console command string, e.g. "stat fps", "show collision".'),
  }),
});

const editorBuildReflectionCaptures = bridgeTool({
  name: "editor_build_reflection_captures",
  domain: "editor",
  description:
    "Bake every reflection-capture cubemap into the CURRENT level's build data — the " +
    "headless equivalent of Build > Build Reflection Captures. Fixes the editor-vs-PIE " +
    "divergence where a reflective scene (e.g. a glossy floor) looks correct in the editor " +
    "viewport (which renders a transient live re-capture) but washed-out grey in PIE / cooked " +
    "builds (which read the SERIALIZED MapBuildData). A stale bake shows the on-screen " +
    "'REFLECTION CAPTURES NEED TO BE REBUILT' warning. Run this after authoring or editing " +
    "reflective materials/geometry, then re-check in PIE. Blocked during PIE; no dry_run.",
  input: z.object({
    save: z
      .boolean()
      .optional()
      .describe(
        "Persist the dirtied MapBuildData (+ map) to disk so PIE/cooked builds pick up the " +
          "bake. Default true.",
      ),
  }),
});

/** Host-project root for project-coupled helpers (unified log, UBT builds). */
function resolveProjectRoot(): string | undefined {
  return process.env.UNREAL_PROJECT_ROOT || undefined;
}

// Live Coding hot-patch. Custom handler reproduces the worktree-mode guard.
const editorLiveCodingCompile = defineTool({
  name: "editor_live_coding_compile",
  domain: "editor",
  description:
    "Trigger a Live Coding compile (Ctrl+Alt+F11): recompiles C++ and hot-patches " +
    "it into the running editor. Returns the final compile result " +
    "(success/no-changes/failure) when done or after a 600s timeout. The compile " +
    "runs on the editor game thread; the bridge stays responsive throughout, so " +
    "mcp_status keeps answering (result.live_coding_in_progress=true) and a " +
    "concurrent caller can tell 'compiling' from 'crashed' (GAP-060). NOT available " +
    "in isolated/worktree mode (UNREAL_MCP_ISOLATED set) — use editor_build_game_target there.",
  input: z.object({}),
  handler: (_args, ctx) => {
    // UNREAL_MCP_ISOLATED marks isolated/worktree mode — Live Coding
    // patches the running editor (built from the main project's Source/), so a
    // worktree's changes would not be compiled.
    if (process.env.UNREAL_MCP_ISOLATED) {
      return {
        success: false,
        message:
          "live_coding_compile is NOT available in isolated/worktree mode. " +
          "Live Coding hot-patches the running editor which uses the main " +
          "project's Source/ — your changes are in an isolated worktree and " +
          "would not be compiled. Use editor_build_game_target() instead, which runs " +
          "a full UBT build against your worktree's files.",
      };
    }
    return ctx.conn.sendCommand("editor_live_coding_compile", {});
  },
});

// Standalone UBT build of the host project's Game target. Runs on the host,
// not the bridge.
const editorBuildGameTarget = defineTool({
  name: "editor_build_game_target",
  domain: "editor",
  description:
    "Run a full UBT build of the host project's Game target (NOT Editor). Standalone " +
    "offline build — does not need the editor running. Project resolution: project_root " +
    "arg, else UNREAL_PROJECT_ROOT. Engine via UNREAL_ENGINE_ROOT.",
  input: z.object({
    project_root: z
      .string()
      .default("")
      .describe("Optional override for the project root. Pass the worktree path in isolated mode."),
    target: z
      .string()
      .default("")
      .describe("Optional UBT target name override (defaults to the .uproject stem)."),
  }),
  handler: (args) => {
    let candidate = args.project_root.trim();
    // Reject Unix-only paths (e.g. "/workspace" from a container) when the
    // build host is Windows — the build runs on the host and needs a host
    // path. On a POSIX host, "/..." is a native path and passes through.
    if (
      process.platform === "win32" &&
      candidate &&
      candidate.startsWith("/") &&
      !candidate.startsWith("/c/") &&
      !candidate.startsWith("/C/")
    ) {
      log.warn(
        `build_game_target: ignoring non-Windows path '${candidate}', using env-resolved root`,
      );
      candidate = "";
    }
    const root = candidate || resolveProjectRoot();
    if (!root) {
      return {
        success: false,
        error: "No project root configured.",
        error_code: "invalid_argument",
        error_hint:
          "Pass project_root, or set UNREAL_PROJECT_ROOT to the host UE project root " +
          "(the directory containing the .uproject). The host launcher script normally exports it.",
      };
    }

    let uprojects: string[];
    try {
      uprojects = readdirSync(root)
        .filter((f) => f.toLowerCase().endsWith(".uproject"))
        .map((f) => join(root, f));
    } catch {
      uprojects = [];
    }
    if (uprojects.length !== 1) {
      return {
        success: false,
        error: `Expected exactly one .uproject in '${root}', found ${uprojects.length}.`,
        error_code: "invalid_argument",
        error_hint:
          "Point project_root / UNREAL_PROJECT_ROOT at the directory that contains the " +
          "host project's single .uproject file.",
      };
    }
    const uprojectWin = uprojects[0]!.replace(/\//g, "\\");
    const targetName =
      args.target.trim() || basename(uprojectWin).replace(/\.uproject$/i, "");

    const engineRoot = process.env.UNREAL_ENGINE_ROOT || "";
    const ubtBat = engineRoot
      ? join(engineRoot, "Engine", "Build", "BatchFiles", "Build.bat")
      : "";
    const ubtBatExists = (() => {
      try {
        return !!ubtBat && statSync(ubtBat).isFile();
      } catch {
        return false;
      }
    })();
    if (!ubtBatExists) {
      return {
        success: false,
        error:
          "Unreal engine not located (UNREAL_ENGINE_ROOT unset or has no Engine/Build/BatchFiles/Build.bat).",
        error_code: "invalid_argument",
        error_hint:
          "Set UNREAL_ENGINE_ROOT to the engine install root used by this project " +
          "(the directory containing Engine/). The host launcher script normally exports it.",
      };
    }
    const cmd = `"${ubtBat}" ${targetName} Win64 Development "${uprojectWin}" -waitmutex`;

    log.info(`build_game_target: building ${targetName} against ${uprojectWin}`);

    const result = spawnSync(cmd, {
      shell: true,
      encoding: "utf8",
      timeout: 600_000,
      maxBuffer: 64 * 1024 * 1024,
    });

    if (result.error) {
      const anyErr = result.error as NodeJS.ErrnoException;
      if (anyErr.code === "ETIMEDOUT" || result.signal === "SIGTERM") {
        return { success: false, message: "Build timed out after 10 minutes" };
      }
      log.error(`build_game_target error: ${anyErr.message}`);
      return { success: false, message: anyErr.message };
    }

    // Trim output to last 200 lines to avoid flooding context.
    const stdoutLines = (result.stdout ?? "").trim().split(/\r?\n/);
    const stdoutTail =
      stdoutLines.length > 200
        ? ["... (truncated)", ...stdoutLines.slice(-200)].join("\n")
        : (result.stdout ?? "").trim();

    if (result.status === 0) {
      return {
        success: true,
        message: "Build succeeded (Game target)",
        output: stdoutTail,
      };
    }
    return {
      success: false,
      message: `Build FAILED (exit ${result.status})`,
      output: stdoutTail,
      stderr: result.stderr ? (result.stderr as string).trim().slice(-2000) : "",
    };
  },
});

/** Path to the host project's MCP_Unified.log, or undefined if no root. */
function unifiedLogPath(): string | undefined {
  const root = resolveProjectRoot();
  if (!root) return undefined;
  return join(root, "Saved", "Logs", "MCP_Unified.log");
}

/** Read all lines (keeping no trailing newline split) — empty if missing. */
function readLogLines(path: string): string[] {
  try {
    const text = readFileSync(path, "utf8");
    // Mirror Python readlines(): split keeping line content; drop a trailing
    // empty element from a final newline so counts match.
    const parts = text.split(/(?<=\n)/);
    if (parts.length && parts[parts.length - 1] === "") parts.pop();
    return parts;
  } catch (e) {
    const err = e as NodeJS.ErrnoException;
    if (err.code === "ENOENT") return [];
    log.error(`read_logs: failed to read ${path}: ${err.message}`);
    return [];
  }
}

/** Extract sequence number from a log line like "[42] 2026-...". */
function parseSeq(line: string): number | undefined {
  if (line.startsWith("[")) {
    const end = line.indexOf("]");
    if (end > 1) {
      const n = Number.parseInt(line.slice(1, end), 10);
      if (Number.isFinite(n) && /^\d+$/.test(line.slice(1, end).trim())) return n;
    }
  }
  return undefined;
}

/** Extract (SOURCE, Category) from a line's "[SOURCE:Category]" tag. */
function parseSourceAndCategory(line: string): [string, string] {
  let scanFrom = 0;
  if (line.startsWith("[")) {
    const seqEnd = line.indexOf("]");
    if (seqEnd > 0) scanFrom = seqEnd + 1;
  }
  const bracket = line.indexOf("[", scanFrom);
  if (bracket < 0) return ["", ""];
  const end = line.indexOf("]", bracket + 1);
  if (end <= bracket) return ["", ""];
  const inner = line.slice(bracket + 1, end);
  const colon = inner.indexOf(":");
  if (colon <= 0) return [inner.trim(), ""];
  return [inner.slice(0, colon).trim(), inner.slice(colon + 1).trim()];
}

// Read the unified MCP log. Runs on the host (filesystem), not the bridge.
const editorReadLogs = defineTool({
  name: "editor_read_logs",
  domain: "editor",
  description:
    "Read the unified MCP log — a single pane into the editor (every UE_LOG, PIE " +
    "event, compile result, MCP command, tagged [SOURCE:Category]). Filters: sources " +
    "(SOURCE half), category (Category half), grep. Pagination (in order): since_seq>0 " +
    "(cursor), offset>=0 (random access), else tail. Returns lines, total_lines, returned, cursor, file.",
  input: z.object({
    tail: z
      .number()
      .int()
      .default(500)
      .describe("Number of lines from the end of the file (default mode)."),
    offset: z
      .number()
      .int()
      .default(-1)
      .describe("0-based line offset for pagination. -1 means use tail mode."),
    limit: z
      .number()
      .int()
      .default(500)
      .describe("Max lines when using offset mode (default 500, max 5000)."),
    since_seq: z
      .number()
      .int()
      .default(0)
      .describe("Return lines with sequence > this value (opaque cursor). 0 disables."),
    sources: z
      .string()
      .default("")
      .describe('Comma-separated SOURCE filter (e.g. "PIE,LIVECODING").'),
    category: z
      .string()
      .default("")
      .describe('Comma-separated UE log-category filter (e.g. "LogTemp,LogBlueprint"), substring, case-insensitive.'),
    grep: z
      .string()
      .default("")
      .describe("Only return lines containing this substring (case-insensitive)."),
  }),
  annotations: { readOnlyHint: true },
  handler: (args) => {
    const logPath = unifiedLogPath();
    if (!logPath) {
      return {
        success: false,
        error: "No project root configured — cannot locate MCP_Unified.log.",
        error_code: "invalid_argument",
        error_hint:
          "Set UNREAL_PROJECT_ROOT to the host UE project root (the directory " +
          "containing Saved/Logs/). The host launcher script normally exports it.",
      };
    }
    let lines = readLogLines(logPath);
    const total = lines.length;

    // --- Apply source filter ---
    if (args.sources.trim()) {
      const allowed = new Set(
        args.sources
          .split(",")
          .map((s) => s.trim().toUpperCase())
          .filter((s) => s),
      );
      if (allowed.size) {
        lines = lines.filter((line) => {
          const [src] = parseSourceAndCategory(line);
          return allowed.has(src.toUpperCase());
        });
      }
    }

    // --- Apply category filter (distinct from sources) ---
    if (args.category.trim()) {
      const catTerms = args.category
        .split(",")
        .map((c) => c.trim().toLowerCase())
        .filter((c) => c);
      if (catTerms.length) {
        lines = lines.filter((line) => {
          const [, cat] = parseSourceAndCategory(line);
          const catLower = cat.toLowerCase();
          return catTerms.some((term) => catLower.includes(term));
        });
      }
    }

    // --- Apply grep filter ---
    if (args.grep.trim()) {
      const grepLower = args.grep.trim().toLowerCase();
      lines = lines.filter((l) => l.toLowerCase().includes(grepLower));
    }

    // --- Pagination ---
    const limit = Math.min(Math.max(args.limit, 1), 5000);
    const tail = Math.min(Math.max(args.tail, 1), 5000);

    let resultLines: string[];
    if (args.since_seq > 0) {
      // Mode 1: cursor-based — all lines with seq > since_seq.
      resultLines = lines.filter((line) => {
        const seq = parseSeq(line);
        return seq !== undefined && seq > args.since_seq;
      });
      if (resultLines.length > 5000) resultLines = resultLines.slice(-5000);
    } else if (args.offset >= 0) {
      // Mode 2: offset-based pagination.
      resultLines = lines.slice(args.offset, args.offset + limit);
    } else {
      // Mode 3: tail (default).
      resultLines = tail < lines.length ? lines.slice(-tail) : lines;
    }

    // --- Extract cursor (highest sequence in batch) ---
    let cursor = 0;
    for (let i = resultLines.length - 1; i >= 0; i--) {
      const seq = parseSeq(resultLines[i]!);
      if (seq !== undefined) {
        cursor = seq;
        break;
      }
    }

    // Strip trailing newlines from each line.
    resultLines = resultLines.map((l) => l.replace(/[\n\r]+$/, ""));

    return {
      lines: resultLines,
      total_lines: total,
      returned: resultLines.length,
      cursor,
      file: logPath,
    };
  },
});

export const editorTools: ToolDef[] = [
  editorPerfSnapshot,
  editorContentBrowserRefresh,
  editorWindowScreenshot,
  editorScreenshot,
  editorViewportGetCamera,
  editorConsoleExec,
  editorBuildReflectionCaptures,
  editorLiveCodingCompile,
  editorBuildGameTarget,
  editorReadLogs,
];
