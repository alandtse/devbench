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

## Benchmarking & structured tests (planned)

Console primitives are validated for scripted tests over MCP/REST: turn (`player.setangle z`),
reposition-along-heading (`player.setpos` from `getpos`+`getangle`), free camera (`tfc`). Gaps
that need dedicated tooling:

- **`camera` tool** (`RE::PlayerCamera`) — force 1st/3rd person, set FOV, and drive the
  **auto-vanity orbit** (camera circling the player). The orbit auto-sweeps every view angle, so
  it's the ideal benchmark camera; console can only nudge `fAutoVanityModeDelay` (idle-triggered).
- **`scenario` runner** — one call runs a typed step list in-process with frame-accurate timing:
  `load`/`coc`, `waitUntil` (playerLoaded / menu-closed) and `waitSettled` (frametime variance
  under threshold — "coc in, wait till settled"), `repeat`, `measure`. Avoids client-side HTTP
  jitter → reproducible benchmarks.
- **`measure` primitive** — sample frametime over a window → min/avg/p95/p99 (the benchmark
  output), and/or correlate with **Tracy** zones around each window.

Client-driven sequencing (issue command, poll inspect/events, sleep, repeat) works today for
ad-hoc tests (validated: load → rotate ×4 with verify); the above is for repeatable,
timing-precise benchmarks.
