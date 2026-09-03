#!/usr/bin/env node
import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { CallToolRequestSchema, ListToolsRequestSchema } from "@modelcontextprotocol/sdk/types.js";

import { callTool, GameUnavailableError, listTools } from "./proxy.js";
import { resolveTarget } from "./runtime.js";
import { printSetupSnippet } from "./setup.js";

function parseArgs(argv: string[]): { game?: string; install?: string; setup: boolean } {
  let game: string | undefined;
  let install: string | undefined;
  let setup = false;
  for (let i = 0; i < argv.length; i++) {
    switch (argv[i]) {
      case "--game":
        game = argv[++i];
        break;
      case "--install":
        install = argv[++i];
        break;
      case "setup":
        setup = true;
        break;
    }
  }
  return { game, install, setup };
}

async function main(): Promise<void> {
  const args = parseArgs(process.argv.slice(2));
  const target = resolveTarget(args);

  if (args.setup) {
    printSetupSnippet(process.execPath === process.argv[0] ? process.argv[1] : process.argv[0], args.game ?? "se");
    return;
  }

  const server = new Server(
    { name: "devbench-bridge", version: "0.1.0" },
    { capabilities: { tools: {} } }
  );

  server.setRequestHandler(ListToolsRequestSchema, async () => {
    try {
      const tools = await listTools(target);
      return { tools: tools.map((t) => ({ name: t.name, description: t.description, inputSchema: t.inputSchema })) };
    } catch (e) {
      if (e instanceof GameUnavailableError) {
        // No game running yet: report an empty tool set rather than failing the whole
        // MCP session — the client stays connected and can retry once the game is up.
        return { tools: [] };
      }
      throw e;
    }
  });

  server.setRequestHandler(CallToolRequestSchema, async (request) => {
    try {
      const result = await callTool(target, request.params.name, (request.params.arguments ?? {}) as Record<string, unknown>);
      return { content: [{ type: "text", text: JSON.stringify(result) }] };
    } catch (e) {
      const message = e instanceof GameUnavailableError ? `game not running (target: ${target.label})` : (e as Error).message;
      return { content: [{ type: "text", text: JSON.stringify({ ok: false, reason: message }) }], isError: true };
    }
  });

  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((e) => {
  console.error(`devbench-bridge: ${(e as Error).message}`);
  process.exit(1);
});
