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

### 3. Batch console sequences (`batch` tool)
Run an ordered sequence of console commands. **Skyrim supports this natively** via
`bat <file>` (reads `Data/<file>.txt` line by line) — so the plan is to lean on that: accept a
JSON array of commands, write a temp bat file under `Data`, and invoke `bat`, OR enqueue the
commands in order through the existing fenced `ExecuteCommand` path. Optional per-command or
whole-batch capture. Useful for scripted test setup (position the player, set weather, spawn
actors, then assert).

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
`127.0.0.1` only. JSON under `SKSE/Plugins/devbench/` (read at `kDataLoaded` before
`Server::Start()`). This mirrors how CS gates its server per-runtime, and is the prerequisite
for letting users turn the bench on/off without a rebuild.
- **TODO: GUI integration** — an in-game menu (ImGui, as CS has) to toggle enable/port live and
  show connected clients, rather than edit-file-then-relaunch. Config file first; GUI later.

## Foundation (discussed, not yet built)
- **`eval` primitive + `search_api` discovery** — thin-but-powerful surface (the
  agentic-renderdoc model): let an agent run script against live RE/game state and discover the
  callable surface, rather than a bespoke tool per operation. Builds on `MainThread::RunAndWait`.
- **Cross-plugin C-ABI** — let *other* SKSE mods register tools into devbench over a versioned
  C ABI (JSON-string in/out + sink callback), the original "general test bench" goal.
- **`EventBus` SSE stream** — `GET /api/events/stream` alongside the `?since=N` poll.
- **Back-port the hook-side fence to Community Shaders' `RemoteControl`** (CS-side) — its shipping
  `ConsoleLogCapture` still uses the read-time slice that we proved breaks on spam.
