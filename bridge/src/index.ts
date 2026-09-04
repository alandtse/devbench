#!/usr/bin/env node
import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";

import { callTool, GameUnavailableError, listTools } from "./proxy.js";
import { isSupportedGame, resolveTarget, type Target } from "./runtime.js";
import { printSetupSnippet } from "./setup.js";

const AVAILABILITY_POLL_MS = 3_000;

// A client typically caches tools/list, so a game that comes up (or goes down)
// after connection needs an explicit nudge to be noticed. `report` is also called
// from the tools/list handler itself, so the client's own first call establishes
// the baseline immediately instead of racing the first poll tick.
function createAvailabilityTracker(
  server: Server,
): (available: boolean) => void {
  let lastAvailable: boolean | undefined;
  return (available: boolean) => {
    if (lastAvailable !== undefined && lastAvailable !== available) {
      void server.notification({ method: "notifications/tools/list_changed" });
    }
    lastAvailable = available;
  };
}

function watchAvailability(
  target: Target,
  report: (available: boolean) => void,
): void {
  setInterval(() => {
    void listTools(target)
      .then(() => true)
      .catch((e) => !(e instanceof GameUnavailableError))
      .then(report);
  }, AVAILABILITY_POLL_MS).unref();
}

// A compiled standalone executable's embedded entry script lives under this
// virtual path; a plain `node dist/index.js` invocation does not.
function isCompiledExecutable(): boolean {
  return process.argv[1]?.includes("~BUN") ?? false;
}

function parseArgs(argv: string[]): {
  game?: string;
  install?: string;
  setup: boolean;
} {
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
  const reportAvailability = createAvailabilityTracker(server);
  watchAvailability(target, reportAvailability);

  server.setRequestHandler(ListToolsRequestSchema, async () => {
    try {
      const tools = await listTools(target);
      reportAvailability(true);
      return {
        tools: tools.map((t) => ({
          name: t.name,
          description: t.description,
          inputSchema: t.inputSchema,
        })),
      };
    } catch (e) {
      if (e instanceof GameUnavailableError) {
        // No game running yet: report an empty tool set rather than failing the whole
        // MCP session — the client stays connected and can retry once the game is up.
        reportAvailability(false);
        return { tools: [] };
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
      );
      return {
        content: [{ type: "text", text: JSON.stringify(result ?? null) }],
      };
    } catch (e) {
      const message =
        e instanceof GameUnavailableError
          ? `game not running (target: ${target.label})`
          : (e as Error).message;
      return {
        content: [
          {
            type: "text",
            text: JSON.stringify({ ok: false, reason: message }),
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
