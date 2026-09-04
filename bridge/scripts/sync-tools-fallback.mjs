#!/usr/bin/env node
// Regenerates src/tools-fallback.json from a live devbench instance's real
// GET /api/tools response. Run this after any change to devbench's core tool
// registry (src/Tools.cpp, src/Capture.cpp, src/HostApi.cpp, src/ToolRegistry.h) --
// CI fails the build if those files change without this one, see
// scripts/check-tools-fallback-sync.mjs.
//
// Usage: node scripts/sync-tools-fallback.mjs [http://127.0.0.1:8920]

import { writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

// Consumer-registered extension tools (openshaders.*, companionexpeditions.*,
// ...) use a dotted "consumer.verb" name by convention, but that convention
// isn't enforced anywhere devbench's C-ABI RegisterTool is called -- a mod
// could register an undotted name, which a bare dot-filter would then
// silently bake into devbench's own "core" fallback. Filtering on this
// explicit allowlist instead means a rogue extension can never get in;
// update it (and re-run this script) whenever a core tool is added/removed.
const CORE_TOOL_NAMES = new Set([
  "menu",
  "console",
  "scenario",
  "inspect",
  "game",
  "camera",
  "papyrus",
  "capture",
  "record",
  "recordings",
  "wait",
  "sleep",
  "mcp_bridge_setup",
  "ping",
]);

const base = process.argv[2] ?? "http://127.0.0.1:8920";
const res = await fetch(`${base}/api/tools`);
if (!res.ok) {
  throw new Error(`GET ${base}/api/tools -> ${res.status}`);
}
const body = await res.json();

const liveNames = new Set(body.tools.map((t) => t.name));
for (const expected of CORE_TOOL_NAMES) {
  if (!liveNames.has(expected)) {
    console.warn(
      `WARNING: expected core tool "${expected}" not found live -- renamed or removed?`,
    );
  }
}
for (const t of body.tools) {
  if (!t.name.includes(".") && !CORE_TOOL_NAMES.has(t.name)) {
    console.warn(
      `WARNING: undotted tool "${t.name}" isn't in CORE_TOOL_NAMES -- ` +
        "add it there if it's a real core tool, or investigate if it's a mod that should use a dotted name.",
    );
  }
}

const core = body.tools
  .filter((t) => CORE_TOOL_NAMES.has(t.name))
  .map(({ name, description, inputSchema, readOnly }) => ({
    name,
    description,
    inputSchema,
    ...(readOnly ? { readOnly } : {}),
  }));

const outPath = fileURLToPath(
  new URL("../src/tools-fallback.json", import.meta.url),
);
writeFileSync(outPath, JSON.stringify({ tools: core }, null, 2) + "\n");
console.log(`Wrote ${core.length} core tools to ${outPath}`);
