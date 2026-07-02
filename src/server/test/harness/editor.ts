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
