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

// Explicit allowlist, not a "no dot in the name" filter -- devbench's C-ABI
// RegisterTool never enforces the dotted "consumer.verb" naming convention
// extension tools otherwise follow, so a bare filter could admit a rogue one.
// Update this set (and re-run) whenever a core tool is added or removed.
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
