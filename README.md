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

## Scripted tests & benchmarking

The bench is enough to drive **reproducible, scripted test sequences** today by chaining tool
calls from any client (here over REST; the MCP `tools/call` equivalents are identical). This is
the exact battery used to validate the action primitives — load a save, settle, rotate in place,
walk, and free the camera:

```jsonc
// 1. Load by name — checkForMods=false skips the "depends on missing mods" modal
POST /api/tool/game     {"action":"load","name":"Save215"}
// 2. Wait until in-world before acting
poll GET /api/tool/inspect   until .playerLoaded == true        // ~5s
// 3. Rotate 90° x4, 3s apart, verifying each step
for z in [0, 90, 180, 270]:
  POST /api/tool/console {"cmd":"player.setangle z " + z}
  POST /api/tool/console {"cmd":"player.getangle z"}            // -> "GetAngle: Z >> 90.00"
  sleep 3
// 4. Walk forward 300u along the current heading z:
//    read pos, then setpos to (x + d*sin z, y + d*cos z)
POST /api/tool/console  {"cmd":"player.setpos x " + (x + 300*sin(z))}
POST /api/tool/console  {"cmd":"player.setpos y " + (y + 300*cos(z))}
// 5. Free camera for a screenshot/benchmark sweep
POST /api/tool/console  {"cmd":"tfc"}
```

`console` returns only the lines its own command produced (a hook-side capture fence), so the
`getangle`/`getpos` reads are reliable even under log spam. Client-driven chaining like this
works now for ad-hoc tests; a server-side **`scenario` runner** and **record→replay** mode (see
[ROADMAP](ROADMAP.md)) will make these one call with frame-accurate timing.

## License

The **devbench plugin** is **GPL-3.0** (`COPYING`) with the standard Skyrim **Modding Exception
+ GPL-3.0 §7 linking exception** (`EXCEPTIONS`) — the same grant Community Shaders and other SKSE
mods carry. It lets the plugin link against the proprietary game code it modifies ("Modded Code")
and against the **Modding Libraries** it builds on — **CommonLibSSE-NG** and **cpp-mcp** (both
MIT) — without those linked parts becoming GPL-covered.

The cross-plugin **API glue is separately MIT** and **carries no copyleft effect**:
`include/DevBenchAPI.h`, `DevBenchAPI.cpp`, and `DevBenchAPI.LICENSE.txt`. **Any** SKSE plugin —
*including closed-source / non-GPL mods* — may vendor those files (via the `devbench-api` vcpkg
port) to talk to devbench with **zero GPL obligation**. This mirrors the **MergeMapper /
SkyrimVRESL** convention: the integration header is permissively licensed precisely so the whole
modding community can depend on it regardless of their own license.
