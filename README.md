# devbench

A standalone SKSE plugin that hosts a **general test bench for Skyrim mod development**:
an in-process server that exposes mod functionality to **AI agents (MCP)** and to **plain
HTTP clients (REST)** through one endpoint, on `127.0.0.1`.

It is transport-agnostic by design. A `ToolRegistry` is the single source of truth; the MCP
and REST adapters _reflect_ it, so a tool registered once is reachable over both protocols
automatically. Other SKSE mods register their own tools through a small C ABI, turning
devbench into a shared bench rather than a per-mod server.

## What devbench does for developers

devbench lets an **AI agent, a test script, or CI drive and measure a running Skyrim** — no
alt-tabbing, no hand-clicking. One local endpoint, MCP for agents and REST for scripts, and a
shared registry every mod can extend.

- **Automated testing.** Run console commands and read their output, inspect live
  engine/plugin state, answer menus, load saves — then chain them with the **`scenario`** tool:
  a timed step list that waits on _real_ events (`waitFor postLoadGame`) instead of guessed
  sleeps, can `repeat`, and returns a per-step transcript. Steps dispatch **any registered
  tool** (including ones your mod adds), so a scenario can flip your feature mid-run and assert
  the result.
- **Record / replay.** **`record`** captures a manual play-through — trajectory, point-of-view,
  cell transitions (doors/`coc`), and the console commands you typed — into a portable file you
  replay deterministically: by hand, on a hotkey, or unattended on load (`autoRun`). The entry
  point (the save you loaded) is captured too, so a replay re-establishes the scene first.
- **Performance / A-B testing.** A recorded play-through replays the **same path and conditions
  every run**, so an A/B shader/feature comparison measures the _change_, not your hands.
  devbench publishes **semantic `EventBus` events** — `record.started`/`record.stopped`,
  `scene.cellLoaded`, `menu`, `lifecycle` — so a profiler client can align a capture to those
  boundaries; pair it with a Tracy-instrumented build (via the `tracy` MCP) to attribute
  frametime/GPU cost to a specific moment, toggle a feature through its tool, replay the same
  recipe, and diff the numbers.
- **Why wire it into your mod.** Register your mod's operations as devbench tools over a tiny
  **MIT** C-ABI (`RegisterTool` / `EmitEvent`) and they instantly appear on **both** MCP and
  REST — so AI assistants (Claude, Cursor), test scripts, and CI can toggle your features, read
  your state, trigger your captures, and benchmark you, all through one shared bench. You write
  the handler; discovery, JSON-Schema docs, and both transports come for free. Because every mod
  registers into the **same** server, one agent session can orchestrate across your whole load
  order. [Open Shaders](https://github.com/alandtse/open-shaders) does this today.

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
game process — that is acceptable for a _local dev bench_ but it must never be bound to a
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
Missing → auto-created with defaults. Invalid → defaults (logged). All keys are optional.

```jsonc
{
  "enabled": true, // start the MCP/REST server at all (default true)
  "port": 8920, // localhost port for /mcp and /api. Default is per-runtime — SE/AE 8920, VR 8921 — so a fixed MCP client URL is stable; set explicitly to override. Iterates only if the chosen port is busy (rare; logged), bound port in runtime.json.
  "logLevel": "info", // trace|debug|info|warn|error (default "info")

  // In-game hotkeys (DXScanCode integers; 0 = disabled). Ignored while the console is open.
  "recordHotkey": 0, // toggle record start/stop
  "recordHotkeyShift": false, // require Shift held with recordHotkey
  "replayHotkey": 0, // replay a recording
  "replayHotkeyShift": false, // require Shift held with replayHotkey
  "replayPath": "", // recording to replay; empty = most recent
  "replayRestoreScene": true, // hotkey replay re-establishes the recorded scene
  "recordIntervalMs": 10, // record: pose sample period in ms (min 10); per-call intervalMs overrides

  // Autorun: replay a recording once on the first load of the session (unattended benchmark).
  "autoRunPath": "", // recording to replay on first postLoadGame; empty = off
  "autoRunRestoreScene": true, // autorun loads the recording's entry save first

  // Settle delay (ms) after a restore-load before the replayed trajectory runs.
  // Hardware-dependent; not baked into the portable recording file.
  "loadSettleMs": 3000,

  // Scene coupling: how strictly a replay reproduces the recorded entry, by how long
  // before record-start the save/coc was brokered (stored per recipe as entryPoint.ageMs).
  //   age <= couplingAnchorMs : "anchored" — restore the entry + re-apply recorded time/weather
  //   age <= couplingCellMs   : "cell"     — restore the entry
  //   else                    : "worldspace" — skip the restore; only assert the worldspace
  // A recipe may override the tier/thresholds in its meta.coupling block.
  "couplingAnchorMs": 10000,
  "couplingCellMs": 60000,

  // A raw coc/cow can stream between scenes without the loading-screen teardown some mods
  // need to free resources (→ CTD). When true, a coc/cow restore first bounces through
  // cleanTransitionCell to force a clean loading screen. Save-loads already tear down.
  "cleanTransition": true,
  "cleanTransitionCell": "QASmoke",
}
```

`enabled: false` skips starting the server entirely. Bind address is fixed to `127.0.0.1`.
The port is **deterministic per runtime** — **SE/AE `8920`, VR `8921`** — so a fixed MCP client URL
never moves; set `port` explicitly to override. If the chosen port is already taken (e.g. a second
instance of the same runtime), devbench iterates to the next free port (logged) and writes the bound
port to `Data/SKSE/Plugins/devbench/runtime.json` (`{ "port": N }`) for discovery.

## Connect an MCP client

devbench serves **streamable-HTTP MCP at `POST http://127.0.0.1:<port>/mcp`** — most clients connect
natively, no extra process:

```sh
# Claude Code — register both games; the one that's running has live tools, the other shows disconnected
claude mcp add --transport http devbench-se http://127.0.0.1:8920/mcp   # SE/AE
claude mcp add --transport http devbench-vr http://127.0.0.1:8921/mcp   # VR
```

For a **stdio-only** client, bridge with the off-the-shelf `mcp-remote` (no devbench-specific binary):
`{ "command": "npx", "args": ["-y", "mcp-remote", "http://127.0.0.1:8920/mcp", "--allow-http"] }`. Plain
HTTP scripts/CI use REST (`/api/tool/<name>`) and need no client config. devbench advertises
`tools.listChanged` and emits `notifications/tools/list_changed`, so tools that register after connect
(a mod loading, or the game finishing load) surface without reconnecting.

## Built-in tools

All tools are reachable over both MCP (`tools/call`) and REST (`POST /api/tool/<name>`).

| Tool       | What it does                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `ping`     | Self-test. Returns `{ "ok": true }`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| `console`  | Run a Skyrim console command. `action='exec'` queues `command` on the main thread. With `capture=true`, fences it between marker commands; `action='read'` then slices ConsoleLog's buffer between the markers and returns `{ markersFound, lines:[…] }`. Useful for `getav`, `getpos`, `help`, etc.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| `inspect`  | Read live game/plugin state. Runs on the main thread and returns synchronously. `kind='state'` → `{ plugin, version, vr, playerLoaded, frame }`. `'vm'` → Papyrus VM health `{ loadedTypes, attachedScripts, arrays, runningStacks, frozenStacks, overstressed }`. `'scene'` → player context `{ cell, worldspace, location, position, gameHour, daysPassed, weather }`. `'mods'` → active load order `{ count, lightCount, total, plugins:[{index, name}], lightPlugins:[…] }` (full and light plugins are separate index spaces; the env fingerprint a repro/CI run pins against). `'player'` → player snapshot `{ name, level, sex, gold, race, actorValues:{health,magicka,stamina,carryWeight each {current,max}}, equipped:{right,left,ammo} }`. `'inventory'` → items held by the player (or a container `formId`) `{ owner, count, items:[{formId, name, formType, count, value, weight, equipped}] }` (filters: `formType`, `limit`). `'quests'` → journal (running/completed) `{ count, quests:[{formId, name, stage, type, active, completed, objectives:[{index, text, state}]}] }` (`limit`). `'effects'` → active magic effects on the player (or an actor `formId`) `{ target, count, activeEffects:[{spell, effect, magnitude, duration, elapsed}] }`. `'refs'` → identify reference(s) sharing one shape `{ formId, formType, name, editorId, base, position }`: pass `formId` for one form, `selected=true` for the console/crosshair ref (set via `prid`), or neither to enumerate loaded refs in the grid (filters: `formType`, `radius`, `limit`). A consumer mod can add a custom `kind` via the C-ABI `RegisterToolExtension` (e.g. load-timing data); `kind='extensions'` lists the registered kinds + descriptors and `kind=<registered>` dispatches to it. |
| `game`     | Save/load and list saves. `action='list'` enumerates the saves directory. `'loadLast'` loads the most recent save (a settled real-game state — avoids `coc`'s heavy new-game init). `'load'`/`'save'` take a `'name'`. All mutating actions are fire-and-forget; watch `lifecycle` events for completion.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| `menu`     | Inspect, open, answer, or dismiss menus. `'list'` → `{ openMenus, messageBoxOpen }`. `'describe'` → active `MessageBoxMenu` body + buttons. `'accept'` → answer a `MessageBoxMenu` by button index (runs its callback; this is how you clear a Yes/No modal). `'open'` → show a menu by name via the UI queue (`kShow`); opens hub menus from a plain name (`TweenMenu`, `Journal Menu`, `MagicMenu`, …); context menus needing a target ref (`ContainerMenu`/`BarterMenu`/`BookMenu`) won't open this way. `'close'` → hide a menu by name via the UI queue (`kHide`). `'invoke'` → dispatch to a consumer-registered menu handler by `name` (a mod exposes its menu's interaction via the C-ABI `RegisterMenuHandler` instead of adding its own tool, keeping the surface to this one tool); `'list'` returns those names under `registered`, and `'describe'` with a `name` returns that handler's descriptor.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| `papyrus`  | Inspect the live Papyrus surface and invoke functions, returning the value. `'list'` → loaded script class names `{ total, returned, truncated, scripts }` (optional `filter`, `limit`). `'describe'` `{ script }` → that class's `{ globalFunctions, memberFunctions, properties }`, each function with params + return type. `'call'` `{ script, function, args?, self?, timeoutMs? }` runs a function via the VM and returns `{ called, returned, returnedType }` — unlike console `cgf`, the return value comes back (e.g. `Utility.GetCurrentGameTime` → a Float). Pass `self` for a **member** call: `{ "form": "0x14 \| editorId" }` targets any form, or `"selected"` uses the console/crosshair ref (set via `prid`); without `self`, only globals/native are callable. Args and returns support bool/number/string, `{ "form": … }` (a form return resolves to `{ formId, formType, editorId, name }`), and arrays of scalars. Omitted trailing **optional** params are padded to the type's neutral default (`None`/`0`/`0.0`/`false`/`""`) — the VM doesn't fill them, and a short arg list otherwise makes reference ops (`MoveTo`, `Disable`, `Kill`) silently no-op; pass explicit values for any optional whose real default isn't neutral (e.g. `StringUtil.Substring`'s `-1`).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| `scenario` | Run a timed sequence of steps server-side and return a per-step transcript. Steps: `tool` (dispatch any registered tool), `wait` (fixed ms), `waitFor` (block on a Skyrim event), `waitUntil` (poll live state). Optional: `repeat` (≤1000), `continueOnError`. See [Scripted tests](#scripted-tests) below.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| `camera`   | Read or set the player camera. `'get'` → `{ pov, freeCam, camX/Y/Z, camPitch/camYaw }`. `'setPov'` → switch first/third/vanity. `'freecam'` `{ on }` → toggle the free camera. `'drive'` `{ x,y,z,pitch,yaw }` → set the free-cam transform (position exact; used for fixed/reproducible benchmark viewpoints).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| `record`   | Capture a manual play-through as a replayable scenario. `'start'` samples player pose + POV every `intervalMs` (default from `recordIntervalMs`) and captures a scene manifest; mid-record cell transitions (doors/`coc`) and typed console commands are captured too. `'stop'` writes the trajectory to `Data/SKSE/Plugins/devbench/recordings/recording_<stamp>.json`. `'status'` reports progress. `'replay'` plays a recording back (`restoreScene` re-establishes the entry save/cell first). A recipe's coupling tier is the producer's signal; a consumer can override it — `coupling` forces a looser tier (`worldspace` skips the restore) and `force` turns a scene mismatch into a reported warning instead of an abort, so you can run a recipe generally; replay returns the effective `coupling`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |

Other mods add their own tools via the C ABI (see [Use devbench from your mod](#use-devbench-from-your-mod)).

## Record, replay, and autorun

The `record` tool captures a manual play-through as a replayable scenario file:

1. Load a save or `coc` to the scene you want to record.
2. Call `record` with `action='start'` (or press the `recordHotkey`). devbench samples the
   player pose + point-of-view every `intervalMs` ms (default from `recordIntervalMs`, min 10)
   on a background thread, captures a one-time scene manifest, and notes the entry point (the
   save loaded or cell `coc`'d to — captured even for saves/loads you do through the menu).
3. Play through the scene — your movement, point-of-view changes, cell transitions
   (doors/`coc`/`cow`), and any console commands you type are all captured. Then call `record`
   with `action='stop'` (or press the hotkey again). The trajectory is written to
   `Data/SKSE/Plugins/devbench/recordings/recording_<stamp>.json` and the path is returned.
4. Replay with `record action='replay' path='<file>'`. With `restoreScene=true` (default for
   hotkey replay), devbench first re-establishes the entry point (loads the save / `coc`s the
   cell) and waits for the player before running the trajectory. How tightly it reproduces the
   entry depends on the **coupling tier** (anchored / cell / worldspace), chosen from how long
   before recording the entry was set up — see `couplingAnchorMs`/`couplingCellMs`. Before the
   trajectory runs, devbench **asserts** you're in the recorded worldspace/cell and aborts on a
   mismatch (e.g. `coc` ambiguity landing you in the wrong worldspace), so a replay can't quietly
   benchmark the wrong scene. A `coc`/`cow` restore bounces through a neutral cell first
   (`cleanTransition`) to force a loading-screen teardown that some mods need to avoid a CTD.

A recipe can pin or tune its own coupling in the recording file's `meta.coupling` block,
overriding the global config thresholds:

```jsonc
"meta": { "coupling": { "tier": "anchored" } }                 // force a tier, or
"meta": { "coupling": { "anchorMs": 5000, "cellMs": 30000 } }  // tune the age thresholds
```

**In-game hotkeys** (configured in `config.json`, opt-in, both default to disabled):

- `recordHotkey` — DXScanCode integer; toggles record start/stop. Ignored while the console is
  open. Optionally require Shift with `recordHotkeyShift: true`.
- `replayHotkey` — replays the recording at `replayPath` (or the most recent recording if
  empty). Optionally require Shift with `replayHotkeyShift: true`. `replayRestoreScene`
  controls whether the scene is re-established first.

**Autorun** (`autoRunPath`) replays a recording once on the first `postLoadGame` of the session
with no client connected and no keypress — a fully unattended benchmark. Set
`autoRunRestoreScene: true` (default) to have devbench load the recorded entry save first.

HUD notifications confirm hotkey actions (record started, record stopped, replay started).

## Scripted tests

The **`scenario`** tool runs a timed step list server-side and returns a per-step transcript —
one call replaces hand-chained requests with frame-accurate timing. Each step is a `tool`
dispatch (any registered tool), a fixed `wait`, an event-driven **`waitFor`**, or a state-poll
`waitUntil`. **Prefer `waitFor`** — it keys off the _actual_ Skyrim event (a load is done when
`lifecycle:postLoadGame` fires) rather than a guessed sleep. This is a validated battery — load,
wait for the load event, settle, rotate in place, then free the camera:

```jsonc
POST /api/tool/scenario          // MCP: tools/call name=scenario — identical body
{
  "steps": [
    { "tool": "game",    "args": { "action": "load", "name": "Save215" } },
    { "waitFor": "postLoadGame", "timeoutMs": 60000 },   // EVENT, not a guessed sleep
    { "waitUntil": "playerLoaded" },                     // belt-and-suspenders state poll
    { "tool": "console", "args": { "command": "player.setangle z 0" } },
    { "wait": 3000 },                                    // pacing with no event → fixed wait
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

`waitFor` shorthands: lifecycle (`postLoadGame`/`saveGame`/`newGame`/`preLoadGame`/
`dataLoaded`/`deleteGame`), or `menuClosed`/`menuOpened` with a `name` (e.g. wait for
`LoadingMenu` to close, or dismiss a modal then wait for it gone); the general form is
`{ "topic": "...", "match": { ... } }`. Add `repeat` (≤1000) to loop the list and
`continueOnError` to keep going past a failed step.

`waitUntil` conditions: `playerLoaded`, `noModal`, `noMenu`.

**Steps dispatch any registered tool — including tools other mods add over the C ABI.** A
consumer mod such as [Open Shaders](https://github.com/alandtse/open-shaders) (a fork of
[Community Shaders](https://www.nexusmods.com/skyrimspecialedition/mods/180419)) can register
a `feature` tool so `{ "tool": "feature", "args": { "action": "toggle", "shortName": "..." } }`
becomes a valid scenario step — letting a test flip a feature mid-run.

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

- **Copy the two API files directly** — `DevBenchAPI.h` + `DevBenchAPI.cpp` into your plugin
  and build them; no vcpkg involved. Simplest bootstrap, fully MIT.

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

To add a sub-capability **under an existing base tool** — instead of a whole new tool — register an
extension keyed by a string (keeps the agent-facing surface small). Opted-in base tools: `menu` and
`inspect`. E.g. expose load-timing data as a custom `inspect` kind:

```cpp
if (dvb->GetBuildNumber() >= 10500)                                 // RegisterToolExtension: 1.5.0+
    dvb->RegisterToolExtension("inspect", "yourmod.loadtimes",
                               R"({"description":"per-plugin load timings"})", &YourHandler, ctx);
```

Driven as `inspect kind="yourmod.loadtimes"` (same handler contract — it receives the full base-tool
args), discovered via `inspect kind=extensions`. For menus, `RegisterToolExtension("menu", name, …)`
(or the older `RegisterMenuHandler`, 1.4.0+) registers a `menu invoke name=<name>` handler, listed
under `menu list`'s `registered`. Opening/closing/listing menus already works generically — only
custom interaction/data needs a handler.

### Design your tools the agentic-renderdoc way

devbench follows the **[agentic-renderdoc](https://github.com/EdenLabs/agentic-renderdoc#why-this-design)**
model: a _thin but powerful_ surface an agent can drive, where a call **returns the value**, not
just an ack. Match that when you register, so an agent gets a coherent bench rather than a pile of
one-off verbs:

- **Return data, not "ok."** A read should answer the question (`{ "loaded": true, "fps": 58 }`),
  not `{ "queued": true }`. Run on the main thread via SKSE's `TaskInterface` and return the
  result synchronously (devbench's own `inspect` does this).
- **Few powerful tools over many narrow ones.** Prefer one `shadercache` tool with an `action`
  enum to four verbs. A general primitive (an `eval`-style entry into your subsystem) beats a
  tool per operation.
- **Self-describe.** Put a real `inputSchema` and a clear `description` on every tool — that
  _is_ the MCP schema and the REST docs; it's how an agent discovers what you offer cold.
- **Make failure legible.** Validate inputs and return an actionable error (what was wrong + how
  to list valid values), rather than silently succeeding.

### Events

`EmitEvent(topic, payload)` publishes to the **same bus** every listener already reads (MCP
notifications + REST `/api/events?since=N`) — your events are first-class, delivered exactly like
devbench's built-in `menu`/`lifecycle` events. devbench can't tell which mod emitted an event (the
interface is shared), so **namespace your topics** the way you namespace tool names —
`yourmod.somethingHappened` — to make the origin clear and avoid collisions with other mods.

## Performance data

devbench publishes semantic **`EventBus` events** — `record.started`/`record.stopped`,
`scene.cellLoaded`, `menu`, and `lifecycle` — so a client can align a capture to those
boundaries (and the `scenario` tool returns per-step timings synchronously) — but devbench does
not collect or serve profiling data itself.
Frametime and GPU metrics are left to dedicated clients: pair devbench with a Tracy-instrumented
mod (using the `tracy` MCP) or any other profiler to annotate captures with what the bench was
doing. See [ROADMAP](ROADMAP.md) for the planned `measure` primitive.

## License

[GPL-3.0-or-later](COPYING) WITH [Modding Exception AND GPL-3.0 Linking Exception (with Corresponding Source)](EXCEPTIONS).
Specifically, the Modded Code is Skyrim (and its variants) and Modding Libraries include [SKSE](https://skse.silverlock.org/), Commonlib (and variants), and Windows.

The cross-plugin **API glue is separately MIT** and **carries no copyleft effect**:
`include/DevBenchAPI.h`, `DevBenchAPI.cpp`, and `DevBenchAPI.LICENSE.txt`. **Any** SKSE plugin —
_including closed-source / non-GPL mods_ — may vendor those files (via the `devbench-api` vcpkg
port) to talk to devbench with **zero GPL obligation**.
