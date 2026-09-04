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

const base = process.argv[2] ?? "http://127.0.0.1:8920";
const res = await fetch(`${base}/api/tools`);
if (!res.ok) {
  throw new Error(`GET ${base}/api/tools -> ${res.status}`);
}
const body = await res.json();

// Consumer-registered extension tools (openshaders.*, companionexpeditions.*,
// ...) use a dotted "consumer.verb" name; every core devbench tool is a bare
// identifier. Filtering on that convention -- rather than a hardcoded name
// list -- means a newly added core tool is picked up automatically.
const core = body.tools
  .filter((t) => !t.name.includes("."))
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
