import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const transport = new StdioClientTransport({
  command: "E:/SteamLibrary/steamapps/common/Skyrim Special Edition/Data/SKSE/Plugins/devbench/devbench-bridge.exe",
  args: ["--game", "se"],
});

const client = new Client({ name: "smoke-test-live", version: "0.0.0" });
await client.connect(transport);

const tools = await client.listTools();
console.log("tool count:", tools.tools.length);
console.log("has 'wait':", tools.tools.some((t) => t.name === "wait"));

const before = await client.callTool({ name: "inspect", arguments: { kind: "scene" } });
console.log("before:", JSON.stringify(before));

const waitResult = await client.callTool({ name: "wait", arguments: { hours: 1 } });
console.log("wait result:", JSON.stringify(waitResult));

const after = await client.callTool({ name: "inspect", arguments: { kind: "scene" } });
console.log("after:", JSON.stringify(after));

await client.close();
process.exit(0);
