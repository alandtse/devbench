// Throwaway local mock of devbench's REST surface, for testing the bridge's proxy logic
// without a live Skyrim process. Not part of the shipped bridge.
import { createServer } from "node:http";

const server = createServer((req, res) => {
  if (req.method === "GET" && req.url === "/api/tools") {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(
      JSON.stringify({
        tools: [
          {
            name: "ping",
            description: "Self-test.",
            inputSchema: { type: "object", properties: {} },
            readOnly: true,
          },
          {
            name: "wait",
            description: "Advance time.",
            inputSchema: {
              type: "object",
              properties: { hours: { type: "integer" } },
            },
            readOnly: false,
          },
        ],
        mcp_bridge: {
          exePath: "C:/fake/devbench-bridge.exe",
          args: ["--game", "se"],
          mcpJsonSnippet: {},
          installCommand: "fake",
        },
      }),
    );
    return;
  }
  const m = req.url?.match(/^\/api\/tool\/([^/]+)$/);
  if (req.method === "POST" && m) {
    let body = "";
    req.on("data", (c) => (body += c));
    req.on("end", () => {
      const args = body ? JSON.parse(body) : {};
      res.writeHead(200, { "Content-Type": "application/json" });
      if (m[1] === "wait") {
        res.end(JSON.stringify({ completed: true, hours: args.hours ?? 0 }));
      } else {
        res.end(JSON.stringify({ ok: true }));
      }
    });
    return;
  }
  res.writeHead(404);
  res.end();
});

const port = 18920;
server.listen(port, "127.0.0.1", () => {
  console.error(`mock devbench listening on ${port}`);
});
