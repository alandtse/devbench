# devbench roadmap

Status: **core validated** — MCP (`/mcp`) + REST (`/api/*`) on one localhost port; `ToolRegistry`
with generic adapters; `console` with a buffer-read fenced capture (reads `ConsoleLog::buffer` and
slices the lines between two marker commands — no `VPrint` detour, because that function is SEH'd
and a `write_branch` on it CTDs the console; see `ConsoleLogCapture`); `inspect` via the
value-returning `MainThread::RunAndWait`.

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

### 4. Blocking menu / message-box handling (`menu` tool) — **DONE for MessageBoxMenu (AE-validated)**

Read and dismiss/accept modal menus that **block gameplay** — especially during a new game
(intro sequence, alternate-start dialogs, `MessageBoxMenu`, `RaceSex Menu`). Without this,
automated new-game → in-world flows stall on a popup.

- Read: `menu list`/`describe` — currently-open menus via `RE::UI`; `MessageBoxMenu` body +
  buttons read directly off `MessageBoxMenu::GetCurrentMessageBoxData()`, no GFx scrape.
- Act: `menu accept`/`close` via `MessageBoxMenu::SelectOption(index)` — answers + dismisses with
  no `QueueMessage` detour. `menu open` is the symmetric `kShow` of `close` — `UIMessageQueue`
  instantiates a registered menu via its factory, so a plain name opens hub menus (`TweenMenu`,
  `Journal Menu`, `MagicMenu`, …); context menus needing a target ref are out of scope for the
  bare-name path. **Validated live on AE 1.6.1170** (read a "missing content / Continue
  loading?" Yes/No box, answered it, box dismissed). **SE** should resolve via the same id `519819`;
  **VR** is blocked on `skyrim_vr_address_library` PR #121 adding the queue id (see Modal handling).
- Not yet reversed: other modal menus (`RaceSex Menu`, alternate-start) need their own
  callback/button RE.

## Milestone: Community Shaders parity → deprecate the built-in MCP server — **DONE (Open Shaders PR #66)**

End state reached: Open Shaders no longer embeds its own MCP server — cpp-mcp + the `RemoteControl`
transport (and the `extern/cpp-mcp` submodule) are removed, and CS registers its tools into devbench
over the cross-plugin C-ABI under the `openshaders.*` namespace. devbench is the single endpoint.
Validated live on 1.6.1170 (tools callable over MCP/REST; reversible toggle + events confirmed).

CS `RemoteControl` tools → devbench bridge (all `openshaders.*`, C-ABI, exception-contained,
main-thread-marshaled):

| CS tool                                    | Status                                                                                                  |
| ------------------------------------------ | ------------------------------------------------------------------------------------------------------- |
| `console`                                  | **Owned by devbench** (better hook-side fence) — not ported.                                            |
| `inspect` (state + shadercache read)       | **Done** — `openshaders.inspect`.                                                                       |
| `feature` (list/get/set/reset/toggle)      | **Done** — `openshaders.feature` (reversible toggle, flip computed main-thread).                        |
| `shadercache` (clear / deleteDisk)         | **Done** — `openshaders.shadercache` (`ShaderCache::Clear`/`DeleteDiskCache`). Read stays on `inspect`. |
| `capture` (renderdoc/screenshot)           | **Done** — `openshaders.capture`.                                                                       |
| `abtest` (status/start/stop/clear/diff)    | **Done** — `openshaders.abtest`.                                                                        |
| `settings` (save/load/reset global config) | **Done** — `openshaders.settings` (`State::Save`/`Load`).                                               |
| shader-recompile events                    | **Done** — `openshaders.shaderRecompiled` from `CompilationSet::Complete`.                              |

Steps (all complete): (1) C-ABI + consumer header; (2) `DevBenchBridge` registers all CS-domain
tools/events; (3)/(4) CS's embedded server **removed outright** (no deprecation flag needed —
devbench is the only path). Tool semantics/inputSchemas preserved under the namespace prefix.

Settings & UI disposition — **done**: CS's MCP transport settings (enable/port/bindAddress) deleted
(devbench owns transport via its `config.json`); CS's `RemoteControl` in-game menu is now a
read-only **bridge-status panel** (devbench presence + build, registered tools, bound port from
`runtime.json`).

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
  - **First increment — the `papyrus` tool — done.** `list`/`describe` are the `search_api`
    discovery surface for the Papyrus VM (loaded classes, function signatures); `call` invokes a
    **global** function via `DispatchStaticCall` or a **member** function on any form (`self`) and
    — unlike console `cgf` — **returns the value** (a custom `IStackCallbackFunctor` bridged to a
    condition variable turns the VM's async callback back into a synchronous result). This is the
    value-returning, Papyrus-native slice; a native binding-registry over RE/render state (where
    Papyrus can't see) remains the larger eval goal. A Papyrus runner was evaluated as the
    _general_ eval engine and rejected for that role (async/off-thread, no runtime compilation,
    blind to render state) — it fits as one leaf capability, which is exactly what `papyrus call`
    is.
    - **RE learning (cost a CTD):** a member dispatch needs the form **bound** to a script object
      first. `DispatchMethodCall2(handle, …)` on a bare `GetHandleForObject` handle for an actor
      with no bound object (`Object Table Size: 0`) null-derefs on the VM thread. The fix is the
      engine's own `FindBoundObject` → `CreateObject` → `BindID` dance (see CommonLib
      `PackUnpack`), then `DispatchMethodCall` on the bound `Object`. Validated live on SE 1.6.x:
      `Actor.GetActorValue`/`GetLevel`/`GetDisplayName` on arbitrary NPCs, 10× rapid stress, no
      crash. Member functions bind `self` to the form's **native** script type (via
      `GetScriptObjectType`), so a parent-class method resolves through the hierarchy (an Actor's
      `ObjectReference.GetBaseObject`).
    - **Form args — done (all param types).** A form arg is packed to the function's **declared
      param type**, not the form's native class: `HandleCall` resolves the `IFunction` (walk the
      type's member/global funcs + parents), reads each `GetParam(i)` `TypeInfo`, and binds the
      form to that class (`CreateObject(paramClass)` + `BindID`) so a base-typed param accepts it.
      Without this, `PackHandle` types the arg as the form's native class and the VM's native-arg
      unpack won't upcast, so a base-typed param rejected it. Validated live on SE 1.6.x:
      `ObjectReference.GetItemCount(Gold001)` (param `Form`) → the player's gold; `GetDistance`
      (param `ObjectReference`) → a real distance; exact-type `Actor.IsHostileToActor(Actor)` and
      all scalar/global calls still pass. (Arrays-of-forms args remain the only unsupported shape.)
- **Cross-plugin C-ABI** — **done & validated end-to-end** (`DevBenchAPI`). Confirmed live: CS
  registers its `feature` tool into devbench over the C-ABI and it's callable over both MCP and
  REST (list + mutating toggle round-trip). Two integration fixes were required — init at
  `kPostLoad` (ready before consumers' `kDataLoaded`) and a `nullptr`-sender listener (the
  MergeMapper idiom) so consumer dispatches arrive. **Clients VENDOR the API via the
  `devbench-api` vcpkg port (`DevBench::API`) — never copy the source** (MIT glue; plugin GPL-3.0).
- **`EventBus` SSE stream** — `GET /api/events/stream` alongside the `?since=N` poll.

(Note: an earlier idea to back-port the hook-side capture fence into CS's `RemoteControl` is
**dropped** — devbench owns console capture, and CS's embedded server is being deprecated, so its
read-time-slice capture goes away with the server rather than getting fixed in place.)

## Benchmarking & structured tests

Console primitives are validated for scripted tests over MCP/REST: turn (`player.setangle z`),
reposition-along-heading (`player.setpos` from `getpos`+`getangle`), free camera (`tfc`).

- **HTTP integration test bench (`tests/http/`) — in progress.** Because every tool is reachable
  over `POST /api/tool/<name>`, the whole surface is scriptable as a live integration suite — a
  pytest set that drives a running game and asserts on responses (e.g. `inspect scene` has a cell;
  `papyrus call Utility.GetCurrentGameTime` → a number; `Actor.GetActorValue` on a target actor →
  its health; `menu open TweenMenu` round-trips). A distinct tier from the host-independent
  `devbench-tests` (Catch2, no game): it **discovers the bound port via `runtime.json`** (so it
  follows the 8920→8921 auto-increment across instances) and **skips the whole suite when no game
  is reachable** (and per-test when `playerLoaded` is false or a tool/action is absent in the live
  `/api/tools` schema), so it is CI-safe. Doubles as a template for mod authors to test their own
  C-ABI-registered tools.
- **`scenario` runner — DONE.** One `scenario` call runs a step list server-side and returns a
  per-step transcript. Steps: `tool` (dispatch any registered tool, incl. consumer-registered
  ones), `wait` (fixed ms), **`waitFor`** (block on a Skyrim _event_ from the `EventBus` —
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
  state) at a fixed cadence (≈1 s) while the user plays _manually_, persists the trajectory as a
  `scenario` file, then replays it. Cadence + interpolation tunable; the captured path doubles as
  the `measure` window. Cheapest first cut needs no new engine hooks — poll the existing pose reads
  on a timer and emit a scenario.

### Modal handling — read/answer a blocking MessageBoxMenu (IMPLEMENTED & AE-validated, no hook needed)

Loading a save whose content no longer matches the load order pops a Yes/No `MessageBoxMenu`
that gates the load. Fully reversed (Ghidra-named SE/AE/VR + added to CommonLibVR). Two engine
callbacks drive this dialog:

- **`LoadGameMissingContentCallBack`** — _"…relies on content that is no longer present… Continue
  Loading?"_ (missing masters/plugins). **All runtimes** (anon-ns in SE/VR, global in AE).
- **`LoadGameUnrecognizedContentCallBack`** — _"…search … in the Creations menu"_. **AE-only**
  (Creations). (Also an AE-only `LoadGameMissingContentDownloadCallBack` sibling.)

**No detour, no callsite hook — read & answer entirely through globals + one non-SEH call.**
New CommonLibVR API on `RE::MessageBoxMenu`:

- **`MessageBoxData* GetCurrentMessageBoxData()`** — the displayed box's data (`queue.back()`). Read
  `bodyText` (BSString), `buttonText` (BSTArray<BSString>), `type`, `optionIndexOffset`, `callback`.
  **Text is plain struct data — no GFx movie scrape.**
- **`void SelectOption(std::int32_t a_buttonIndex)`** — answer as if button N was clicked: computes
  `optionIndexOffset + N`, pops+destroys the message via the engine `RemoveMessageFromQueue`
  (id `51426`/`52284` — a plain function, **not** the SEH'd `QueueMessage`), posts `kHide` if the
  queue empties (no SWF is driving the close), then runs `callback->Run(index)`. This is a faithful
  replay of the engine's own `ProcessButtonPress` (the Scaleform "buttonPress" handler).
  **MUST run on the main thread** — it mutates UI/game state and `Run(1)` ("Continue Loading")
  kicks off a real `BGSSaveLoadManager` load.

**Two corrections found during implementation:** (a) the queue id is `519819`/`406362`, **not**
`519818`/`406360` — the latter is the per-type _skip-filter_ byte-table, and using it returned an
empty/garbage array on a displayed box (this was the live bug fixed in CommonLibVR #160). (b) The
`BSTArray<MessageBoxData*>` queue is **not** popped when the box is shown: `ProcessButtonPress`
reads `queue[size-1]` _while displayed_ and only pops on answer (via `RemoveMessageFromQueue`). So
`GetCurrentMessageBoxData()` (`queue.back()`) reliably reads the shown box on **all runtimes** —
strictly better than
`GetCurrentMessageBoxMenu()` (AE-only, id `406361`, absent on SE/VR). The `MenuOpenCloseEvent`
("MessageBoxMenu") open signal is still the right trigger to know _when_ to read.

**Still never detour `MessageBoxMenu::QueueMessage`** (AE `0x94b720`, id `52271`): it is an MSVC
`__try`/SEH function (`UNW_FLAG_UHANDLER`); a `write_branch<5>` relocates its SEH prologue into a
trampoline with no `.pdata` → unwinder fails on the guarded load path → CTD. **This is what
disabled `ModalCapture`.** The read/answer path above touches it not at all, so re-enabling
`ModalCapture` no longer needs that hook — drop the QueueMessage entry detour entirely.

Limitation: `SelectOption` handles one active box; if boxes are stacked it does not re-trigger the
engine's "show next" refresh (the SWF normally drives that). Single-modal is the common case.

### Tracy / profiler integration — emit _markers_, leave _data_ to clients

devbench should **not** hard-depend on Tracy or try to be a profiler data source — mods that are
Tracy-instrumented (Community Shaders) own their zones, and a client can already pull frametime
from a live Tracy connection (the `tracy` MCP) or from the game's own timing. What devbench _is_
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
deliberately _not_ a DSL. The wheel we must not reinvent is JavaScript: do **not** add expressions,
variables, arithmetic (e.g. the `setpos = x + d·sin θ` math), branching, or loops-with-conditions
to the JSON step list. Two escape hatches cover logic-heavy needs instead:

- **client-side composition** — the agent computes values and emits concrete steps (it already has
  a real language); and
- the planned **`eval` primitive** — one powerful "run a script against live RE state" tool (the
  agentic-renderdoc model), which is the right home for computation, not a bespoke step grammar.

Adding _primitives_ (a `camera` tool, `measure`, `waitSettled`, record→replay emitting a step
file) is in-scope; adding _control flow_ to the step list is not.
