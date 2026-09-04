// Prints a ready-to-paste MCP client config snippet. Never writes to any config file
// itself — per the architecture decision, the bridge proposes, a human approves/pastes.

export function printSetupSnippet(
  exePath: string,
  args: { game?: string; install?: string },
): void {
  const name = `devbench-${args.game ?? "custom"}`;
  const cliArgs = args.install
    ? ["--install", args.install]
    : ["--game", args.game ?? "se"];
  const snippet = {
    mcpServers: {
      [name]: {
        command: exePath,
        args: cliArgs,
      },
    },
  };
  console.log(`Add this to your MCP client's config (e.g. .mcp.json):\n`);
  console.log(JSON.stringify(snippet, null, 2));
  console.log(
    `\nThen restart/reload your MCP client to pick up the "${name}" server.`,
  );
}
