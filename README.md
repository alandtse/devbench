# devbench

A standalone SKSE plugin that hosts a **general test bench for Skyrim mod development**:
an in-process server that exposes mod functionality to **AI agents (MCP)** and to **plain
HTTP clients (REST)** through one endpoint, on `127.0.0.1`.

It is transport-agnostic by design. A `ToolRegistry` is the single source of truth; the MCP
and REST adapters *reflect* it, so a tool registered once is reachable over both protocols
automatically. Other SKSE mods register their own tools through a small C ABI, turning
devbench into a shared bench rather than a per-mod server.

## Design

```
            register (in-proc or cross-plugin C-ABI)
 mods ───────────────────────────────────────────────▶  ToolRegistry  ◀── EventBus
                                                              ▲                ▲
                                              ┌───────────────┴──────┐         │
                                         McpAdapter             RestAdapter ───┘
                                         (/mcp, JSON-RPC)       (/api/*, plain HTTP)
                                              └──────────┬───────────┘
                                                  one httplib server, one port
```

- **`ToolRegistry`** — name → `{descriptor, handler}`. Handlers are JSON-in / JSON-out and
  run on the listener thread; handlers that touch game/render state marshal to the main
  thread via `MainThread::RunAndWait`, which **returns the value synchronously** (the
  agentic-renderdoc model: an agent gets data back, not just an ack).
- **`EventBus`** — fan-out for notifications (e.g. shader recompiles): MCP notifications for
  agents, `GET /api/events?since=N` polling or SSE for HTTP clients.
- **Adapters** — generic; neither contains a tool name. The descriptor's JSON Schema doubles
  as the MCP `inputSchema` and the REST `GET /api/tools` documentation.

The tool surface is intentionally **thin and powerful** (an `eval`/`console` primitive plus a
small set of conveniences) rather than a tool per operation.

## Safety

Bound to `127.0.0.1` only. The bench has no auth and can execute arbitrary commands in the
game process — that is acceptable for a *local dev bench* but it must never be bound to a
network-reachable address, and `eval`-class tools are gated behind an explicit enable.

## Build

xmake, C++23, CommonLibSSE-NG (submodule). cpp-mcp is vendored as the `lib/cpp-mcp`
submodule and built in-tree as a static-lib target (`xmake/cpp-mcp.lua`, mirroring
Community Shaders' `cmake/cpp-mcp.cmake`): only its four server TUs are compiled, and a
build-tree header mirror applies two edits — pointing `mcp_message.h` at our
`nlohmann/json.hpp` (single ABI) and adding a public `http()` getter to `mcp_server.h` so
the REST facade can share the MCP port.

```
git submodule update --init --recursive
xmake
# auto-deploy: set SkyrimPluginTargets to ';'-separated game Data dirs before building
```

## Configuration

Headless config (no in-game menu yet) at `Data/SKSE/Plugins/devbench/config.json` — read
once at `kPostLoad` (before any consumer's `kDataLoaded`), then the server starts.
Missing/invalid → defaults. See `config.example.json`:

```json
{ "enabled": true, "port": 8920 }
```

`enabled: false` skips starting the server entirely. Bind address is fixed to `127.0.0.1`.
`port` is the *starting* port: if it's busy (a second instance, etc.) devbench auto-iterates
to the next free port and writes the bound port to `Data/SKSE/Plugins/devbench/runtime.json`
(`{ "port": N }`) so fixed-URL clients can discover it.

## Use devbench from your mod

Register your own MCP/REST tools (and emit events) into the running host over a small C ABI.
Don't copy the API files — vendor the `devbench-api` vcpkg port (the portfile fetches the
MIT-licensed API source from this repo):

```cmake
find_package(devbench-api CONFIG REQUIRED)
target_link_libraries(YourPlugin PRIVATE DevBench::API)
```

After SKSE sends your plugin `kPostLoad`:

```cpp
#include <DevBenchAPI.h>
if (auto* dvb = DevBenchAPI::GetDevBenchInterface001()) {            // null if devbench absent
    dvb->RegisterTool("yourmod.do", R"({"description":"…","inputSchema":{…}})",
                      &YourHandler, yourCtx);                        // handler: C fn ptr + ctx
    dvb->EmitEvent("yourmod.ready", R"({"ok":true})");
}
```

Your tool then appears on both `/mcp` (`tools/list`/`tools/call`) and `/api/tool/<name>`.
Handlers run on the server thread — marshal to the main game thread (SKSE `TaskInterface`) for
anything touching game state. See `include/DevBenchAPI.h` and `cmake/ports/devbench-api/README.md`.

## Built-in tools

`console` (run/capture commands, fenced), `inspect` (live state), `game` (save/load/loadLast/list),
`menu` (list open menus / close a blocking modal), plus a `ping` self-test. Other mods add theirs
via the C ABI above.

## License

GPL-3.0 (see `COPYING`/`EXCEPTIONS`). Dependencies (CommonLibSSE-NG, cpp-mcp) are MIT.
