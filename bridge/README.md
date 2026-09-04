# devbench-bridge

A stdio MCP proxy for [devbench](../README.md). Spawned by your MCP client per-session,
it forwards `tools/list`/`tools/call` to whatever Skyrim SE/AE/VR process is currently
running devbench's REST API — so your MCP connection survives the game restarting
(the normal SKSE dev loop: rebuild → close game → relaunch), which a direct connection
to devbench's own `/mcp` endpoint does not.

It holds no state of its own: no port, no cache, no baked-in tool list. Every call reads
the live port from that install's `Data/SKSE/Plugins/devbench/runtime.json` and proxies
straight through, so it never drifts from whatever devbench build is actually running.
When the game isn't up, tool calls return a clean `{ok:false, reason:"game not running"}`
instead of killing your MCP session — restart the game and keep calling, no reconnect.

## Setup

If devbench is installed, the bridge is already at
`Data/SKSE/Plugins/devbench/devbench-bridge.exe` — nothing to download. Add one entry to
your MCP client's config (Claude Code's `.mcp.json`, Claude Desktop's config, etc.):

```json
{
  "mcpServers": {
    "devbench-se": {
      "command": "C:/path/to/Skyrim Special Edition/Data/SKSE/Plugins/devbench/devbench-bridge.exe",
      "args": ["--game", "se"]
    }
  }
}
```

Running SE and VR at once? Add a second entry pointed at the VR install with `--game vr`
(or `--install <path>` for either, if it's not one of the common Steam locations). Each
is its own bridge process — they don't share state, so both can be used concurrently.

`devbench-bridge setup --game se` prints this snippet for you (never writes to your
client config itself — you paste it in).

You can also get this exact snippet live from a running game: `GET /api/tools`'s
`mcp_bridge` field, or `Data/SKSE/Plugins/devbench/mcp-bridge.json`.

## Flags

- `--game se|vr` — target the default Steam install for that runtime.
- `--install <path>` — target a specific Skyrim install directory (required if devbench
  isn't in one of the default Steam locations, or you're running a non-default copy).
- `setup` — print the `.mcp.json` snippet instead of running as an MCP server.

## Development

```
npm install
npm run build      # tsc, for iterating
npm run compile    # bun build --compile → dist/devbench-bridge.exe (standalone, no Node/Bun required to run it)
```

`test/` holds a throwaway mock devbench server and MCP-client smoke test for exercising
the proxy logic without a live game — not part of the shipped bridge.
