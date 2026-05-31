# devbench roadmap

Status: **core validated** — MCP (`/mcp`) + REST (`/api/*`) on one localhost port; `ToolRegistry`
with generic adapters; `console` with a hook-side fenced capture (validated on a clean flat
profile *and* a spam-heavy VR modlist — isolates a command's output from a multi-million-line
ConsoleLog flood); `inspect` via the value-returning `MainThread::RunAndWait`.

The items below are planned tools/capabilities. Each is one or more `ToolRegistry::Register`
calls, so they appear on both MCP and REST automatically.

## Planned tools

### 1. Save / load game (`game` tool)
Load the last or a named save, trigger a save, and list saves — likely via
`RE::BGSSaveLoadManager`. Console has **no** load command, so this fills a real gap: it gives a
settled, real game state for testing instead of `coc`, which forces heavy new-game init
(script populate, the per-frame NPC-AI spam, long shader compiles). Directly answers the
earlier "can we load the last game?" question.
- Actions: `load` (last/named), `save`, `list`.

### 2. Lifecycle & scene events → `EventBus` (MCP notifications + `GET /api/events`)
Emit events so an agent knows **when** it is safe to act, instead of polling `inspect` (which we
had to do manually while waiting out the load). Sources:
- Load lifecycle via SKSE `MessagingInterface`: data loaded (`kDataLoaded`), new game
  (`kNewGame`), `kPreLoadGame` / `kPostLoadGame`, `kSaveGame`, `kDeleteGame`.
- Loading-screen open/close and other menus via `RE::MenuOpenCloseEvent` (e.g. "Loading Menu").
- Cell / location change via cell-loaded / actor-cell sinks (e.g. `TESCellFullyLoadedEvent`).
Wire each into the existing `EventBus` → already fans out to MCP notifications and the REST
poll/SSE endpoint.

### 3. Batch console sequences — covered by native `bat`, no tool
**Dropped.** Skyrim already runs command sequences natively via `bat <file>` (reads a `.txt`
from the game directory), and that's reachable through the existing `console` tool —
`console exec "bat <name>"`. A dedicated inline `batch` tool was prototyped and removed as
redundant. To script setup, write a `.txt` and `console exec "bat <name>"`.

### 4. Blocking menu / message-box handling (`menu` tool)
Read and dismiss/accept modal menus that **block gameplay** — especially during a new game
(intro sequence, alternate-start dialogs, `MessageBoxMenu`, `RaceSex Menu`). Without this,
automated new-game → in-world flows stall on a popup.
- Read: currently-open menus + message-box text (via `RE::UI` / the menu's GFx movie).
- Act: accept / select option / close (via `RE::UIMessageQueue`, menu button invocation, or the
  message-box callback).

## Milestone: Community Shaders parity → deprecate the built-in MCP server

End state: Community Shaders no longer embeds its own MCP server (cpp-mcp + the
`RemoteControl` transport drop out of CS); instead CS **registers its tools into devbench**
over the cross-plugin C-ABI, and devbench is the single endpoint. This requires the C-ABI
(see Foundation) as the enabler, since CS-specific tools touch `globals::features` / CS
internals that a separate plugin can't reach directly.

Mapping of CS `RemoteControl` tools → devbench:

| CS tool | Kind | Plan |
| --- | --- | --- |
| `console` (exec/read) | generic | **Done** in devbench — with the better hook-side fence. |
| `inspect(state)` | generic | **Done** (extend fields toward CS's as needed). |
| `inspect(shadercache)` | CS-domain | CS registers via C-ABI. |
| `feature` (list/toggle/set/reset) | CS-domain | CS registers via C-ABI. |
| `capture` (renderdoc/screenshot) | CS-domain | CS registers via C-ABI. |
| `abtest` (status/start/stop/clear/diff) | CS-domain | CS registers via C-ABI. |
| shader-recompile events | CS-domain | CS publishes into devbench's `EventBus` via C-ABI. |

Steps: (1) ship the C-ABI in devbench + a small consumer header; (2) add a devbench-registrant
module in CS that registers the CS-domain tools/events; (3) gate CS's built-in server behind a
deprecation flag (default to devbench when present); (4) remove CS's in-process server once
devbench is the established path. Keep tool names/inputSchemas matching CS so existing clients
are unaffected.

## Configuration

devbench is headless (no menu yet), so it needs a small **config file** to control startup —
at minimum `enabled` (start the MCP/REST server at all) and `port` (default 8920); bind stays
`127.0.0.1` only. JSON under `SKSE/Plugins/devbench/` (read at `kPostLoad` before
`Server::Start()`). **Done** (`enabled`/`port`, with port auto-iterate + `runtime.json`); mirrors
how CS gates its server per-runtime, and lets users turn the bench on/off without a rebuild.
- **TODO: GUI integration** — an in-game menu (ImGui, as CS has) to toggle enable/port live and
  show connected clients, rather than edit-file-then-relaunch. Config file first; GUI later.

## Foundation (discussed, not yet built)
- **`eval` primitive + `search_api` discovery** — thin-but-powerful surface (the
  agentic-renderdoc model): let an agent run script against live RE/game state and discover the
  callable surface, rather than a bespoke tool per operation. Builds on `MainThread::RunAndWait`.
- **Cross-plugin C-ABI** — **done & validated end-to-end** (`DevBenchAPI`). Confirmed live: CS
  registers its `feature` tool into devbench over the C-ABI and it's callable over both MCP and
  REST (list + mutating toggle round-trip). Two integration fixes were required — init at
  `kPostLoad` (ready before consumers' `kDataLoaded`) and a `nullptr`-sender listener (the
  MergeMapper idiom) so consumer dispatches arrive. **Clients VENDOR the API via the
  `devbench-api` vcpkg port (`DevBench::API`) — never copy the source** (MIT glue; plugin GPL-3.0).
- **`EventBus` SSE stream** — `GET /api/events/stream` alongside the `?since=N` poll.
- **Back-port the hook-side fence to Community Shaders' `RemoteControl`** (CS-side) — its shipping
  `ConsoleLogCapture` still uses the read-time slice that we proved breaks on spam.

## Benchmarking & structured tests

Console primitives are validated for scripted tests over MCP/REST: turn (`player.setangle z`),
reposition-along-heading (`player.setpos` from `getpos`+`getangle`), free camera (`tfc`).

- **`scenario` runner — DONE.** One `scenario` call runs a step list server-side and returns a
  per-step transcript. Steps: `tool` (dispatch any registered tool, incl. consumer-registered
  ones), `wait` (fixed ms), **`waitFor`** (block on a Skyrim *event* from the `EventBus` —
  lifecycle `postLoadGame`/`saveGame`/… or `menuOpened`/`menuClosed`, or a generic `{topic,
  match}`), and `waitUntil` (poll live state: `playerLoaded`/`noModal`/`noMenu`). `repeat` +
  `continueOnError` top-level. Runs on the listener thread, so it sleeps/polls directly and
  marshals each action to the main thread. Prefer `waitFor` over fixed `wait` — keys off the real
  signal, no guessed sleeps. Avoids client-side HTTP jitter → reproducible.
- **`camera` tool** (`RE::PlayerCamera`) — force 1st/3rd person, set FOV, and drive the
  **auto-vanity orbit** (camera circling the player). The orbit auto-sweeps every view angle, so
  it's the ideal benchmark camera; console can only nudge `fAutoVanityModeDelay` (idle-triggered).
- **`measure` primitive** — sample frametime over a window → min/avg/p95/p99 (the benchmark
  output). Pairs with a `scenario` step (measure during a defined segment).
- **`waitSettled`** — a `scenario` wait that blocks until frametime variance drops under a
  threshold ("coc in, wait till settled"); builds on the same frametime sampling as `measure`.
- **Record → replay** — a recording mode that samples player pose (`getpos`/`getangle` + camera
  state) at a fixed cadence (≈1 s) while the user plays *manually*, persists the trajectory as a
  `scenario` file, then replays it. Cadence + interpolation tunable; the captured path doubles as
  the `measure` window. Cheapest first cut needs no new engine hooks — poll the existing pose reads
  on a timer and emit a scenario.

### Modal handling — read/answer a blocking MessageBoxMenu (deferred; RCA below)

Loading a save whose content no longer matches the load order pops a Yes/No
`MessageBoxMenu` (`LoadGameUnrecognizedContentCallBack`) that gates the load. `menu` can detect it
(`messageBoxOpen`) but **cannot read its text or select a button** — `kHide` does not dismiss a
message box; a button callback must run. For automated benchmarks, **prefer saves compatible with
the current load order** (no modal); this is the edge-case follow-up.

**Do NOT detour `MessageBoxMenu::QueueMessage` at the function entry.** Ghidra-verified on
1.6.1170: that function (AE `0x94b720`, id 52271) is an MSVC `__try`/SEH function with a registered
unwind handler (`UNW_FLAG_UHANDLER`). A `write_branch<5>` relocates its SEH-frame prologue into a
trampoline with no `.pdata`, so the x64 unwinder fails on the exception-guarded load path →
guaranteed CTD (the id and branch alignment are both correct; branch size is irrelevant). This is
why `ConsoleLog::VPrint` hooks fine (no unwind handler) but this one does not.

Safe implementation paths:
- **`GetCurrentMessageBoxMenu()`** (AE id `406361`; absent on SE 1.5.97) off the existing
  `MenuOpenCloseEvent` sink — read the *shown* box with no detour. (The pending-queue `BSTArray` is
  popped when the box is displayed, so polling it races.)
- **Callsite hook** (`stl::write_thunk_call`) at the load flow's call to `QueueMessage` — captures
  the `MessageBoxData*` (→ `bodyText`, `buttonText`, `callback->Run(index)`) without touching the
  SEH'd function body or its `.pdata`.

### Tracy / profiler integration — emit *markers*, leave *data* to clients

devbench should **not** hard-depend on Tracy or try to be a profiler data source — mods that are
Tracy-instrumented (Community Shaders) own their zones, and a client can already pull frametime
from a live Tracy connection (the `tracy` MCP) or from the game's own timing. What devbench *is*
uniquely positioned to add is **semantic markers**: it knows when a scenario step starts/ends and
when a `measure` window opens. So:
- Always emit these as **`EventBus` events** (`scenario.step` begin/end, `measure.window`) — free,
  no new dep, and any client (incl. a Tracy-side tool) can align a capture to them.
- **Optionally**, behind a compile flag (mirroring CS's `TRACY_SUPPORT`), mirror those markers as
  Tracy frame-marks / messages so a `.tracy` capture is annotated with what the bench was doing —
  without devbench collecting or serving any profiling data itself.

## Distribution

Nexus page is live: **[Skyrim SE mod 181326](https://www.nexusmods.com/games/skyrimspecialedition/mods/181326)**.
When releases are turned on (semantic-release is disabled while iterating; first tag will be
1.0.0), port Open Shaders' `nexus-upload.yaml` workflow and wire this mod id so the built archive
auto-uploads. Until then, releases are manual.

- **Release CI bumps the port pin** — on each release, auto-update `cmake/ports/devbench-api`'s
  `REF`/`SHA512` to the new tag (the SkyrimVRESL convention: a CI that updates client port files
  with the latest release hash). Consumers who vendored the port then pull a one-line bump.

## Introspection / control surface

Expose devbench's own state over the same MCP/REST surface (most also belongs in the future UI,
but it's cheap and useful to an agent):
- **`config` tool** (build first) — `get` returns `{enabled, configuredPort, boundPort, logLevel}`;
  `set` applies `logLevel` live (bump to debug, reproduce, read the log) and persists
  `enabled`/`port` to config.json flagged restart-required. High value, low cost.
- **Registrant list** (build next) — record each consumer plugin that requested the C-ABI (sender
  name + reported build + time) in `HostApi` and surface it (e.g. `inspect kind=registrants`):
  "Open Shaders vX registered N tools."
- **Event source tagging** — consumer `EmitEvent`s currently can't be attributed (the interface is
  a shared singleton), so origin relies on the namespaced-topic convention (`yourmod.x`). Since
  there's no release yet, the C-ABI can still evolve freely: give each consumer a per-plugin
  interface instance (or pass caller identity) so devbench stamps a structured `source` into the
  event envelope — robust origin without relying on topic discipline. Same path enables per-tool→
  mod attribution for the registrant list.
- **Connected clients** (UI-leaning, defer) — REST is stateless and only MCP sessions are
  "connected"; listing/kicking other clients is a human/UI concern, low value to the agent that is
  itself the client. Expose a basic active-session count later if cheap.

## Scope guard — keep `scenario` thin; don't grow a scripting language

`scenario` is a **thin sequencer** (dispatch a tool · `wait` · `waitFor` · `waitUntil` · `repeat`),
deliberately *not* a DSL. The wheel we must not reinvent is JavaScript: do **not** add expressions,
variables, arithmetic (e.g. the `setpos = x + d·sin θ` math), branching, or loops-with-conditions
to the JSON step list. Two escape hatches cover logic-heavy needs instead:
- **client-side composition** — the agent computes values and emits concrete steps (it already has
  a real language); and
- the planned **`eval` primitive** — one powerful "run a script against live RE state" tool (the
  agentic-renderdoc model), which is the right home for computation, not a bespoke step grammar.

Adding *primitives* (a `camera` tool, `measure`, `waitSettled`, record→replay emitting a step
file) is in-scope; adding *control flow* to the step list is not.
