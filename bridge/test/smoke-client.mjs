// Throwaway smoke test: spawns the compiled bridge and drives it as a real MCP client
// would, over stdio, against the mock devbench server.
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const transport = new StdioClientTransport({
  command: "E:/Documents/source/repos/devbench/bridge/dist/devbench-bridge.exe",
  args: ["--install", "F:/Temp/fake-install"],
});

const client = new Client({ name: "smoke-test", version: "0.0.0" });
await client.connect(transport);

const tools = await client.listTools();
console.log("tools/list ->", JSON.stringify(tools));

const result = await client.callTool({ name: "wait", arguments: { hours: 1 } });
console.log("tools/call(wait) ->", JSON.stringify(result));

await client.close();
process.exit(0);
