/**
 * Launch / await / tear down a live Unreal Editor for integration tests — TS
 * port of `tests/harness/editor.py`. Headless (`-nullrhi`) by default.
 *
 * Lifecycle: link the plugin into the project → (optionally) build the editor
 * target → spawn the editor → poll `mcp_status` until interactive → yield → quit
 * + tree-kill. Bun's Subprocess + a Windows `taskkill /T` reap child workers
 * (ShaderCompileWorker, CrashReportClient) so nothing lingers on the bridge port.
 */

import { mkdirSync } from "node:fs";
import { join } from "node:path";
import * as env from "./env.ts";
import { probeBridge } from "./bridge.ts";

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

export class EditorLaunchError extends Error {}

/** The bridge port must belong to the editor WE launch. Anything already
 *  listening there — a zombie test editor or another project's live editor —
 *  is machine-level state the test run owns (docs/TESTING.md): stop the owning
 *  process tree, then wait for the socket to fully release so our editor's
 *  listener bind doesn't race the dying one. Best-effort. */
export async function reclaimBridgePort(): Promise<void> {
  if ((await probeBridge()) === "down") return;
  if (process.platform === "win32") {
    const q = Bun.spawnSync([
      "powershell", "-NoProfile", "-Command",
      `(Get-NetTCPConnection -LocalPort ${env.BRIDGE_PORT} -State Listen -ErrorAction SilentlyContinue).OwningProcess`,
    ]);
    const pids = [...new Set(q.stdout.toString().split(/\s+/).filter((t) => /^\d+$/.test(t)))];
    for (const pid of pids) {
      console.warn(
        `bridge port ${env.BRIDGE_PORT} held by pid ${pid} — stopping it so the test editor owns the port`,
      );
      Bun.spawnSync(["taskkill", "/F", "/T", "/PID", pid], { stdout: "ignore", stderr: "ignore" });
    }
  } else {
    Bun.spawnSync(["sh", "-c", `lsof -ti tcp:${env.BRIDGE_PORT} | xargs -r kill -9`], {
      stdout: "ignore",
      stderr: "ignore",
    });
  }
  const deadline = Date.now() + 15_000;
  while (Date.now() < deadline && (await probeBridge()) !== "down") await sleep(500);
  await sleep(500); // settle margin after the socket frees
}

export function ensurePluginLinked(): void {
  const dst = env.pluginDest();
  if (env.isDir(dst)) return;
  if (!env.isDir(env.PLUGIN_SRC)) throw new EditorLaunchError(`plugin source missing: ${env.PLUGIN_SRC}`);
  mkdirSync(join(dst, ".."), { recursive: true });
  const cmd =
    process.platform === "win32"
      ? ["cmd.exe", "/c", "mklink", "/J", dst, env.PLUGIN_SRC]
      : ["ln", "-s", env.PLUGIN_SRC, dst];
  const p = Bun.spawnSync(cmd);
  if (p.exitCode !== 0) throw new EditorLaunchError(`plugin link failed: ${p.stderr.toString()}`);
}

export class EditorSession {
  private proc?: Bun.Subprocess;
  private readonly logDir = join(env.projectDir(), "Saved", "MCPTestLogs");

  async start(build: "auto" | "always" | "never" = "auto"): Promise<this> {
    ensurePluginLinked();
    await reclaimBridgePort();
    mkdirSync(this.logDir, { recursive: true });

    if (build === "always" || (build === "auto" && !env.isBuilt())) {
      const r = Bun.spawnSync(
        [
          env.buildScript(),
          env.editorTarget(),
          "Win64",
          "Development",
          `-Project=${env.uprojectPath()}`,
          "-WaitMutex",
        ],
        { stdout: Bun.file(join(this.logDir, "build.log")), stderr: "pipe" },
      );
      if (r.exitCode !== 0) throw new EditorLaunchError(`build failed (exit ${r.exitCode})`);
    } else if (!env.isBuilt()) {
      throw new EditorLaunchError(`editor target not built (${env.editorModuleDll()}); pass build=auto`);
    }

    const log = join(this.logDir, "editor.log");
    this.proc = Bun.spawn(
      [
        env.editorCmdExe(),
        env.uprojectPath(),
        "-nullrhi", "-nosound", "-unattended", "-nopause", "-nosplash",
        "-stdout", "-FullStdOutLogOutput", `-AbsLog=${log}`,
        "-AutoDeclinePackageRecovery",
      ],
      { stdout: "ignore", stderr: "ignore", cwd: env.engineRoot() },
    );

    await this.awaitReady();
    return this;
  }

  private async awaitReady(): Promise<void> {
    const deadline = Date.now() + env.BOOT_TIMEOUT_S * 1000;
    while (Date.now() < deadline) {
      if (this.proc && this.proc.exitCode !== null) {
        throw new EditorLaunchError(`editor exited during boot (code ${this.proc.exitCode}); see ${this.logDir}`);
      }
      if ((await probeBridge()) === "interactive") return;
      await sleep(1000);
    }
    await this.stop();
    throw new EditorLaunchError(`editor not interactive within ${env.BOOT_TIMEOUT_S}s`);
  }

  async stop(): Promise<void> {
    if (!this.proc) return;
    const pid = this.proc.pid;
    this.proc.kill();
    try {
      await this.proc.exited;
    } catch {
      /* already gone */
    }
    if (process.platform === "win32" && pid) {
      Bun.spawnSync(["taskkill", "/F", "/T", "/PID", String(pid)], { stderr: "ignore", stdout: "ignore" });
    }
    this.proc = undefined;
  }
}
