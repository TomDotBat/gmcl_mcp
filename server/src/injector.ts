// injector.ts — thin wrapper around injector.exe (the native launcher).
// Resolves the executable path, runs status/inject/eject, and parses the JSON
// the injector prints on stdout.

import { execFile } from "node:child_process";
import { promisify } from "node:util";
import path from "node:path";
import { fileURLToPath } from "node:url";
import fs from "node:fs";

const execFileAsync = promisify(execFile);
const __dirname = path.dirname(fileURLToPath(import.meta.url));

export interface InjectorResult {
  ok: boolean;
  running?: boolean;
  pid?: number;
  injected?: boolean;
  alreadyInjected?: boolean;
  pipeAlive?: boolean;
  ejected?: boolean;
  error?: string;
}

// Search order for injector.exe: env override, then a few common build output
// locations relative to the server package.
function resolveInjectorPath(): string | null {
  const candidates = [
    process.env.GMOD_MCP_INJECTOR,
    path.resolve(__dirname, "..", "..", "build", "bin", "Release", "injector.exe"),
    path.resolve(__dirname, "..", "..", "build", "bin", "Debug", "injector.exe"),
    path.resolve(__dirname, "..", "bin", "injector.exe"),
  ].filter(Boolean) as string[];
  for (const c of candidates) {
    if (fs.existsSync(c)) return c;
  }
  return null;
}

async function run(args: string[]): Promise<InjectorResult> {
  const exe = resolveInjectorPath();
  if (!exe) {
    return {
      ok: false,
      error:
        "injector.exe not found. Build the native project (premake5 vs2022 && build Release|x64) or set GMOD_MCP_INJECTOR to its path.",
    };
  }
  try {
    const { stdout } = await execFileAsync(exe, args, { windowsHide: true });
    return JSON.parse(stdout.trim()) as InjectorResult;
  } catch (e: any) {
    // The injector prints JSON even on logical failure; non-zero exit still has stdout.
    if (e?.stdout) {
      try { return JSON.parse(String(e.stdout).trim()) as InjectorResult; } catch { /* fall through */ }
    }
    return { ok: false, error: String(e?.message ?? e) };
  }
}

export const injector = {
  status: () => run(["status"]),
  inject: () => run(["inject"]),
  eject: () => run(["eject"]),
};
