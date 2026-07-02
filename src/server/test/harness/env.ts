/**
 * Engine/project path + env resolution for the integration harness — TS port of
 * `tests/harness/config.py`. Mirrors the same env vars so a machine set up to run
 * the server already runs the tests.
 *
 *   UNREAL_ENGINE_ROOT   engine root (dir containing Engine/)   [required for editor tests]
 *   UE_MCP_TEST_PROJECT  host project dir   [default: <repo>/tests/fixtures/TestProject]
 *   UE_MCP_BRIDGE_PORT   default 55557
 *   UE_MCP_BOOT_TIMEOUT  seconds to wait for interactive (default 420)
 *   UE_MCP_LIVE=1        let editor.test.ts launch its own editor
 */

import { existsSync, readdirSync, statSync } from "node:fs";
import { join, resolve, basename } from "node:path";

// src/server/test/harness/env.ts → repo root is four parents up.
export const REPO_ROOT = resolve(import.meta.dir, "..", "..", "..", "..");
export const PLUGIN_SRC = join(REPO_ROOT, "src", "Plugin", "UnrealMCP");
const DEFAULT_PROJECT = join(REPO_ROOT, "tests", "fixtures", "TestProject");

const isWin = process.platform === "win32";
const isMac = process.platform === "darwin";
const plat = isWin ? "Win64" : isMac ? "Mac" : "Linux";

export const BRIDGE_HOST = process.env.UE_MCP_BRIDGE_HOST ?? "127.0.0.1";
export const BRIDGE_PORT = Number(process.env.UE_MCP_BRIDGE_PORT ?? "55557");
export const BOOT_TIMEOUT_S = Number(process.env.UE_MCP_BOOT_TIMEOUT ?? "420");

export function engineRoot(): string {
  const root = process.env.UNREAL_ENGINE_ROOT;
  if (!root || !existsSync(join(root, "Engine"))) {
    throw new Error(
      "UNREAL_ENGINE_ROOT is not set / invalid — point it at your Unreal Engine " +
        "install root (the directory containing 'Engine/').",
    );
  }
  return root;
}

const engineBin = () => join(engineRoot(), "Engine", "Binaries", plat);
export const editorCmdExe = () =>
  isWin ? join(engineBin(), "UnrealEditor-Cmd.exe") : join(engineBin(), "UnrealEditor-Cmd");
export const editorGuiExe = () =>
  join(engineBin(), isWin ? "UnrealEditor.exe" : "UnrealEditor");
export const buildScript = () => {
  const bf = join(engineRoot(), "Engine", "Build", "BatchFiles");
  return isWin ? join(bf, "Build.bat") : join(bf, plat, "Build.sh");
};

export const projectDir = () =>
  resolve(process.env.UE_MCP_TEST_PROJECT ?? DEFAULT_PROJECT);

export function uprojectPath(): string {
  const d = projectDir();
  const match = readdirSync(d).find((f) => f.endsWith(".uproject"));
  if (!match) throw new Error(`No .uproject in ${d}`);
  return join(d, match);
}

export const projectName = () => basename(uprojectPath()).replace(/\.uproject$/, "");
export const editorTarget = () => `${projectName()}Editor`;
export const pluginDest = () => join(projectDir(), "Plugins", "UnrealMCP");

export function editorModuleDll(): string {
  const ext = isWin ? ".dll" : isMac ? ".dylib" : ".so";
  return join(projectDir(), "Binaries", plat, `UnrealEditor-${projectName()}${ext}`);
}

export const isBuilt = () => existsSync(editorModuleDll());
export const isDir = (p: string) => existsSync(p) && statSync(p).isDirectory();
