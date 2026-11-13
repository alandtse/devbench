#!/usr/bin/env node
import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";

import { callTool, GameUnavailableError, listTools } from "./proxy.js";
import { isSupportedGame, resolveTarget } from "./runtime.js";
import { printSetupSnippet } from "./setup.js";
import {
  isUnknownToolError,
  parseBridgeArgs,
  RestCallError,
} from "./rest-policy.js";
import toolsFallback from "./tools-fallback.json" with { type: "json" };

interface DevbenchTool {
  name: string;
  description: string;
  inputSchema: Record<string, unknown>;
  readOnly?: boolean;
}

// devbench's REST shape uses a bare `readOnly`; MCP's is `annotations.readOnlyHint`.
// Shared by both tools/list paths so they can't drift out of sync with each other.
function toMcpTool(t: DevbenchTool) {
  return {
    name: t.name,
    description: t.description,
    inputSchema: t.inputSchema,
    ...(t.readOnly ? { annotations: { readOnlyHint: true } } : {}),
  };
}

// A compiled standalone executable's embedded entry script lives under this
// virtual path; a plain `node dist/index.js` invocation does not.
function isCompiledExecutable(): boolean {
  return process.argv[1]?.includes("~BUN") ?? false;
}

async function main(): Promise<void> {
  const args = parseBridgeArgs(process.argv.slice(2));

  if (args.setup) {
    if (!args.install && !isSupportedGame(args.game)) {
      throw new Error(
        "devbench-bridge setup requires --game se|vr or --install <path>.",
      );
    }
    const scriptArgs = isCompiledExecutable() ? [] : [process.argv[1]];
    printSetupSnippet(process.execPath, scriptArgs, args);
    return;
  }

  const target = resolveTarget(args);

  const server = new Server(
    { name: "devbench-bridge", version: "0.1.0" },
    { capabilities: { tools: { listChanged: true } } },
  );

  let advertised = "";
  const signatureOf = (tools: DevbenchTool[]): string =>
    JSON.stringify(
      tools.map(toMcpTool).sort((a, b) => a.name.localeCompare(b.name)),
    );

  const notifyIfToolListChanged = async (): Promise<void> => {
    try {
      if (signatureOf(await listTools(target)) !== advertised) {
        await server.sendToolListChanged();
      }
    } catch (e) {
      if (!(e instanceof GameUnavailableError)) {
        console.error(
          `devbench-bridge: tools/list_changed check failed: ${(e as Error).message}`,
        );
      }
    }
  };

  server.setRequestHandler(ListToolsRequestSchema, async () => {
    try {
      const tools = await listTools(target);
      advertised = signatureOf(tools);
      return { tools: tools.map(toMcpTool) };
    } catch (e) {
      if (e instanceof GameUnavailableError) {
        // A not-yet-running game reports the static fallback, not an empty list --
        // a client typically fetches tools/list only once per session. A call still
        // fails live with GameUnavailableError's own message.
        advertised = signatureOf(toolsFallback.tools);
        return { tools: toolsFallback.tools.map(toMcpTool) };
      }
      throw e;
    }
  });

  server.setRequestHandler(CallToolRequestSchema, async (request) => {
    try {
      const result = await callTool(
        target,
        request.params.name,
        request.params.arguments ?? {},
        args.restTimeoutMs,
      );
      return {
        content: [{ type: "text", text: JSON.stringify(result ?? null) }],
      };
    } catch (e) {
      const message =
        e instanceof GameUnavailableError
          ? `game not running (target: ${target.label})`
          : (e as Error).message;
      // The game restarted (or a provider plugin did not load) since the client
      // fetched tools/list, so it still advertises a tool this game lacks: tell it
      // to fetch the list again.
      if (isUnknownToolError(e)) {
        void notifyIfToolListChanged();
      }
      return {
        content: [
          {
            type: "text",
            text: JSON.stringify({
              ok: false,
              reason: message,
              ...(e instanceof RestCallError ? { restError: e.details() } : {}),
            }),
          },
        ],
        isError: true,
      };
    }
  });

  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((e) => {
  console.error(`devbench-bridge: ${(e as Error).message}`);
  process.exit(1);
});
