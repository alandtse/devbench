#!/usr/bin/env node
// CI guard: fails if a devbench core-tool registration file changed without
// src/tools-fallback.json also changing in the same diff, so the bridge's
// static fallback can't silently drift from the real tool registry. Run
// scripts/sync-tools-fallback.mjs against a live devbench to update it.
//
// Usage: node scripts/check-tools-fallback-sync.mjs <base-ref>

import { execFileSync } from "node:child_process";

const baseRef = process.argv[2];
if (!baseRef) {
  throw new Error("usage: check-tools-fallback-sync.mjs <base-ref>");
}

const REGISTRATION_FILES = [
  "src/Tools.cpp",
  "src/Capture.cpp",
  "src/HostApi.cpp",
  "src/ToolRegistry.h",
];
const FALLBACK_FILE = "bridge/src/tools-fallback.json";

// Run from the repo root (CI does; a local run should too) so the paths below
// match git's own repo-relative output.
const changed = execFileSync(
  "git",
  ["diff", "--name-only", `${baseRef}...HEAD`],
  {
    encoding: "utf-8",
  },
)
  .split("\n")
  .filter(Boolean);

const registrationChanged = REGISTRATION_FILES.some((f) => changed.includes(f));
const fallbackChanged = changed.includes(FALLBACK_FILE);

if (registrationChanged && !fallbackChanged) {
  console.error(
    `A devbench core-tool file changed (${REGISTRATION_FILES.filter((f) => changed.includes(f)).join(", ")}) ` +
      `without ${FALLBACK_FILE}. Run 'node bridge/scripts/sync-tools-fallback.mjs' against a live devbench ` +
      "and commit the result.",
  );
  process.exit(1);
}
console.log("tools-fallback.json sync check passed.");
