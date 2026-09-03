// Translates MCP tools/list + tools/call into devbench's existing REST contract
// (GET /api/tools, POST /api/tool/<name>). No tool schema is cached or baked in —
// every list is fetched live, so the bridge never drifts from whatever DLL build is
// actually running.

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

async function fetchJson(url: string, init?: RequestInit): Promise<unknown> {
  let res: Response;
  try {
    res = await fetch(url, init);
  } catch (e) {
    throw new GameUnavailableError(`devbench not reachable at ${url}: ${(e as Error).message}`);
  }
  const body = await res.json().catch(() => undefined);
  if (!res.ok) {
    const message = (body as { error?: string } | undefined)?.error ?? res.statusText;
    throw new Error(`devbench returned ${res.status}: ${message}`);
  }
  return body;
}

export async function listTools(target: Target): Promise<DevbenchToolDescriptor[]> {
  const base = resolveBaseUrl(target);
  const body = (await fetchJson(`${base}/api/tools`)) as DevbenchToolsResponse;
  return body.tools;
}

export async function callTool(target: Target, name: string, args: Record<string, unknown>): Promise<unknown> {
  const base = resolveBaseUrl(target);
  return fetchJson(`${base}/api/tool/${encodeURIComponent(name)}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(args ?? {}),
  });
}
