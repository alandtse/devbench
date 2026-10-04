#!/usr/bin/env node
// CI guard: fails if a devbench core-tool registration file changed without
// src/tools-fallback.json also changing in the same diff, so the bridge's
// static fallback can't silently drift from the real tool registry. Run
// scripts/sync-tools-fallback.mjs against a live devbench to update it.
//
// Usage: node scripts/check-tools-fallback-sync.mjs <base-ref>

import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";

const baseRef = process.argv[2];
if (!baseRef) {
  throw new Error("usage: check-tools-fallback-sync.mjs <base-ref>");
}

const REGISTRATION_FILES = [
  "src/Tools.cpp",
  "src/Capture.cpp",
  "src/HostApi.cpp",
  "src/KeyboardInput.cpp",
];
// Not every edit to this header changes what tools advertise: only its descriptor
// shape and default schema reach tools/list, so it counts only when a changed code
// line (comments excluded) mentions one of those.
const DESCRIPTOR_FILE = "src/ToolRegistry.h";
const DESCRIPTOR_TOKENS =
  /\b(DefaultInputSchema|ToolDescriptor|inputSchema|readOnly)\b/;
const FALLBACK_FILE = "bridge/src/tools-fallback.json";

// Structural validation, independent of whether a registration file changed --
// the file-touched check below only proves the file moved, not that it's sane.
const fallback = JSON.parse(readFileSync(FALLBACK_FILE, "utf-8"));
if (!Array.isArray(fallback.tools) || fallback.tools.length === 0) {
  console.error(`${FALLBACK_FILE}: "tools" must be a non-empty array.`);
  process.exit(1);
}
for (const t of fallback.tools) {
  if (typeof t.name !== "string" || !t.name) {
    console.error(`${FALLBACK_FILE}: a tool entry is missing a valid "name".`);
    process.exit(1);
  }
  if (typeof t.description !== "string" || !t.description) {
    console.error(
      `${FALLBACK_FILE}: tool "${t.name}" is missing a "description".`,
    );
    process.exit(1);
  }
  if (t.inputSchema?.type !== "object") {
    console.error(
      `${FALLBACK_FILE}: tool "${t.name}" has no inputSchema.type === "object" ` +
        "(would fail MCP's schema validation).",
    );
    process.exit(1);
  }
}

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

function descriptorChanged() {
  if (!changed.includes(DESCRIPTOR_FILE)) return false;
  const diff = execFileSync(
    "git",
    ["diff", "-U0", `${baseRef}...HEAD`, "--", DESCRIPTOR_FILE],
    { encoding: "utf-8" },
  );
  return diff
    .split("\n")
    .filter((l) => /^[+-](?![+-])/.test(l))
    .map((l) => l.slice(1).trim())
    .filter((l) => l && !l.startsWith("//") && !l.startsWith("/*"))
    .some((l) => DESCRIPTOR_TOKENS.test(l));
}

const touchedRegistrationFiles = [
  ...REGISTRATION_FILES.filter((f) => changed.includes(f)),
  ...(descriptorChanged() ? [DESCRIPTOR_FILE] : []),
];
const registrationChanged = touchedRegistrationFiles.length > 0;
const fallbackChanged = changed.includes(FALLBACK_FILE);

if (registrationChanged && !fallbackChanged) {
  console.error(
    `A devbench core-tool file changed (${touchedRegistrationFiles.join(", ")}) ` +
      `without ${FALLBACK_FILE}. Run 'node bridge/scripts/sync-tools-fallback.mjs' against a live devbench ` +
      "and commit the result.",
  );
  process.exit(1);
}
console.log("tools-fallback.json sync check passed.");
