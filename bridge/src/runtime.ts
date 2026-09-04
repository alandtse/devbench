// runtime.json is re-read on every call (never cached), since devbench's port can
// change across a game restart.

import { readFileSync } from "node:fs";
import { join } from "node:path";

export interface Target {
  /** Human-readable label for error messages ("SE", "VR", or the --install path). */
  label: string;
  /** Directory containing runtime.json (…/Data/SKSE/Plugins/devbench). */
  runtimeDir: string;
}

interface RuntimeJson {
  port: number;
  [key: string]: unknown;
}

// Best-effort default Steam library locations; --install covers anything else.
const DEFAULT_SE_INSTALLS = [
  "C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition",
  "C:/SteamLibrary/steamapps/common/Skyrim Special Edition",
  "D:/SteamLibrary/steamapps/common/Skyrim Special Edition",
  "E:/SteamLibrary/steamapps/common/Skyrim Special Edition",
];
const DEFAULT_VR_INSTALLS = [
  "C:/Program Files (x86)/Steam/steamapps/common/SkyrimVR",
  "C:/SteamLibrary/steamapps/common/SkyrimVR",
  "D:/SteamLibrary/steamapps/common/SkyrimVR",
  "E:/SteamLibrary/steamapps/common/SkyrimVR",
];

function runtimeDirFor(installPath: string): string {
  return join(installPath, "Data", "SKSE", "Plugins", "devbench");
}

function firstExisting(candidates: string[]): string | undefined {
  for (const dir of candidates) {
    try {
      readFileSync(join(dir, "runtime.json"), "utf-8");
      return dir;
    } catch {
      // not this one
    }
  }
  return undefined;
}

/** Resolve a Target from parsed CLI args. Throws with a clear message if none found. */
export function resolveTarget(args: {
  game?: string;
  install?: string;
}): Target {
  if (args.install) {
    return { label: args.install, runtimeDir: runtimeDirFor(args.install) };
  }
  if (args.game === "se" || args.game === "vr") {
    const candidates = (
      args.game === "se" ? DEFAULT_SE_INSTALLS : DEFAULT_VR_INSTALLS
    ).map(runtimeDirFor);
    const found = firstExisting(candidates);
    if (!found) {
      throw new Error(
        `Could not find a devbench install for --game ${args.game} in any default Steam ` +
          `location. Pass --install <path-to-Skyrim-folder> instead.`,
      );
    }
    return { label: args.game.toUpperCase(), runtimeDir: found };
  }
  throw new Error("devbench-bridge requires --game se|vr or --install <path>.");
}

/** Read the live port + base URL for a target, fresh every call. */
export function resolveBaseUrl(target: Target): string {
  const raw = readFileSync(join(target.runtimeDir, "runtime.json"), "utf-8");
  const parsed = JSON.parse(raw) as RuntimeJson;
  if (typeof parsed.port !== "number") {
    throw new Error(
      `runtime.json at ${target.runtimeDir} has no numeric "port" field.`,
    );
  }
  return `http://127.0.0.1:${parsed.port}`;
}
