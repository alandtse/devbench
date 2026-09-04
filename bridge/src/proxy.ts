// Every tool list is fetched live (never cached), so the bridge never drifts from
// whatever DLL build is actually running.

import { resolveBaseUrl, type Target } from "./runtime.js";

interface DevbenchToolDescriptor {
  name: string;
  description: string;
  inputSchema: Record<string, unknown>;
  readOnly?: boolean;
}

interface DevbenchToolsResponse {
  tools: DevbenchToolDescriptor[];
  mcp_bridge?: unknown;
}

export class GameUnavailableError extends Error {}

// A stale/wrong port can accept the connection but never respond; bound how long
// a call waits before treating it as unreachable rather than hanging indefinitely.
const FETCH_TIMEOUT_MS = 30_000;

function resolveBaseUrlOrThrowUnavailable(target: Target): string {
  try {
    return resolveBaseUrl(target);
  } catch (e) {
    throw new GameUnavailableError(
      `runtime discovery failed for ${target.label}: ${(e as Error).message}`,
    );
  }
}

async function fetchJson(url: string, init?: RequestInit): Promise<unknown> {
  let res: Response;
  try {
    res = await fetch(url, {
      ...init,
      signal: AbortSignal.timeout(FETCH_TIMEOUT_MS),
    });
  } catch (e) {
    throw new GameUnavailableError(
      `devbench not reachable at ${url}: ${(e as Error).message}`,
    );
  }
  const body: unknown = await res.json().catch(() => undefined);
  if (!res.ok) {
    const message =
      (body as { error?: string } | undefined)?.error ?? res.statusText;
    throw new Error(`devbench returned ${res.status}: ${message}`);
  }
  return body;
}

export async function listTools(
  target: Target,
): Promise<DevbenchToolDescriptor[]> {
  const base = resolveBaseUrlOrThrowUnavailable(target);
  const body = (await fetchJson(`${base}/api/tools`)) as DevbenchToolsResponse;
  if (!Array.isArray(body.tools)) {
    throw new Error(`devbench /api/tools response missing a "tools" array`);
  }
  return body.tools;
}

export async function callTool(
  target: Target,
  name: string,
  args: Record<string, unknown>,
): Promise<unknown> {
  const base = resolveBaseUrlOrThrowUnavailable(target);
  return fetchJson(`${base}/api/tool/${encodeURIComponent(name)}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(args ?? {}),
  });
}
