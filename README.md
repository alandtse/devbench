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
The integration glue (`include/DevBenchAPI.h` + `DevBenchAPI.cpp`) is **MIT** — pick whichever
fits your project:

- **Copy the overlay port** (recommended) — drop `cmake/ports/devbench-api/` into your repo's
  vcpkg overlay and you're done; the portfile fetches the MIT API source (no source copied into
  your tree). This works out of the box — it's exactly what consumers do today:
  ```cmake
  find_package(devbench-api CONFIG REQUIRED)
  target_link_libraries(YourPlugin PRIVATE DevBench::API)
  ```
- **Copy the two API files directly** — `DevBenchAPI.h` + `DevBenchAPI.cpp` into your plugin and
  build them; no vcpkg involved. Simplest bootstrap, fully MIT.

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

### Design your tools the agentic-renderdoc way

devbench follows the **[agentic-renderdoc](https://github.com/EdenLabs/agentic-renderdoc#why-this-design)**
model: a *thin but powerful* surface an agent can drive, where a call **returns the value**, not
just an ack. Match that when you register, so an agent gets a coherent bench rather than a pile of
one-off verbs:

- **Return data, not "ok."** A read should answer the question (`{ "loaded": true, "fps": 58 }`),
  not `{ "queued": true }`. Run on the main thread via SKSE's `TaskInterface` and return the result
  synchronously (devbench's own `inspect` does this).
- **Few powerful tools over many narrow ones.** Prefer one `shadercache` tool with an `action`
  enum to four verbs. A general primitive (an `eval`-style entry into your subsystem) beats a tool
  per operation.
- **Self-describe.** Put a real `inputSchema` and a clear `description` on every tool — that *is*
  the MCP schema and the REST docs; it's how an agent discovers what you offer cold.
- **Make failure legible.** Validate inputs and return an actionable error (what was wrong + how to
  list valid values), rather than silently succeeding.

### Events

`EmitEvent(topic, payload)` publishes to the **same bus** every listener already reads (MCP
notifications + REST `/api/events?since=N`) — your events are first-class, delivered exactly like
devbench's built-in `menu`/`lifecycle` events. devbench can't tell which mod emitted an event (the
interface is shared), so **namespace your topics** the way you namespace tool names —
`yourmod.somethingHappened` — to make the origin clear and avoid collisions with other mods.

## Built-in tools

`console` (run/capture commands, fenced), `inspect` (live state), `game` (save/load/loadLast/list),
`menu` (list open menus / close a blocking modal), `scenario` (timed sequence runner, below), plus
a `ping` self-test. Other mods add theirs via the C ABI above.

## Scripted tests & benchmarking

The **`scenario`** tool runs a timed step list server-side and returns a per-step transcript — one
call replaces hand-chained requests with frame-accurate timing. Each step is a `tool` dispatch
(any registered tool), a fixed `wait`, an event-driven **`waitFor`**, or a state-poll `waitUntil`.
**Prefer `waitFor`** — it keys off the *actual* Skyrim event (a load is done when
`lifecycle:postLoadGame` fires) rather than a guessed sleep. This is the validated battery — load,
wait for the load event, settle, rotate in place, then free the camera:

```jsonc
POST /api/tool/scenario          // MCP: tools/call name=scenario — identical body
{
  "steps": [
    { "tool": "game",    "args": { "action": "load", "name": "Save215" } },
    { "waitFor": "postLoadGame", "timeoutMs": 60000 },   // EVENT, not a guessed sleep
    { "waitUntil": "playerLoaded" },                     // belt-and-suspenders state poll
    { "tool": "console", "args": { "command": "player.setangle z 0" } },
    { "wait": 3000 },                                    // pacing has no event → fixed wait
    { "tool": "console", "args": { "command": "player.setangle z 90" } },
    { "wait": 3000 },
    { "tool": "console", "args": { "command": "player.setangle z 180" } },
    { "wait": 3000 },
    { "tool": "console", "args": { "command": "player.setangle z 270" } },
    { "tool": "console", "args": { "command": "tfc" } }  // free cam for a screenshot sweep
  ]
}
// -> { "ok": true, "stepsRun": 11, "elapsedMs": 14213,
//      "results": [ { "index": 0, "kind": "tool", "ok": true, ... },
//                   { "index": 1, "kind": "waitFor", "satisfied": true, "elapsedMs": 4870 }, ... ] }
```

`waitFor` shorthands: lifecycle (`postLoadGame`/`saveGame`/`newGame`/`preLoadGame`/`dataLoaded`),
or `menuClosed`/`menuOpened` with a `name` (e.g. wait for `LoadingMenu` to close, or dismiss a
modal then wait for it gone); the general form is `{ "topic": "...", "match": { ... } }`. Add
`repeat` (≤1000) to loop the list and `continueOnError` to keep going past a failed step.

**Steps dispatch any registered tool — including tools other mods add over the C ABI.** Open
Shaders (a fork of [Community Shaders](https://www.nexusmods.com/skyrimspecialedition/mods/180419))
is a worked consumer — its
[`src/DevBenchBridge.cpp`](https://github.com/alandtse/open-shaders/blob/dev/src/DevBenchBridge.cpp)
registers a `feature` tool, so `{ "tool": "feature", "args": { "action": "toggle", "shortName":
"..." } }` becomes a valid scenario step — letting a benchmark flip an Open Shaders feature
mid-run. (`console` also still returns only its own command's output via the hook-side
fence, so `getangle`/`getpos` reads stay reliable under log spam.)

Still to come (see [ROADMAP](ROADMAP.md)): a `measure` primitive (frametime over a window),
`waitSettled`, a `camera` tool, and **record→replay** (capture a manual run as a scenario file).

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
