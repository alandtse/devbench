# Implementation Plan — Replay-Checkpoint Frame Capture

**Scope:** devbench (host, `E:\Documents\source\repos\devbench`) + Open Shaders (reference capture provider, `E:\Documents\source\repos\open-shaders`).
**Goal:** capture a frame at deterministic checkpoints during a recording replay, signal reliably when the file is on disk, and return the path plus correlation metadata. All comparison (SSIM, thresholds, goldens, pass/fail) lives in Python.
**Status:** final — produced via supervisor plan (Opus) → adversarial review (Gemini/agy-bridge) → rebuttal/reconciliation → consolidation. Self-contained; supersedes all earlier drafts.

---

## 0. Verified ground truth

Everything below was read in the actual source, not assumed. Anchors are `file:line`.

| Fact                                                                                                          | Anchor                                                                                  |
| ------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------- |
| `RegisterToolExtension` mechanism; base tools keyed case-insensitively                                        | `devbench/src/ToolExtensions.cpp:36-54`                                                 |
| Extension dispatch precedent (`inspect kind=<registered>`)                                                    | `devbench/src/Tools.cpp:1112-1114`                                                      |
| Base-tool descriptor rebuild on registration                                                                  | `devbench/src/Tools.cpp:2001-2016`                                                      |
| Scenario step dispatch; step kinds `pose/wait/waitFor/waitUntil/tool/assert`                                  | `devbench/src/Tools.cpp:1566, 1611, 1616, 1649, 1669, 1687`; unknown-step throw `:1772` |
| Per-step exception containment (a throwing step fails that step, not the run)                                 | `devbench/src/Tools.cpp:1774-1791`                                                      |
| `repeat` loop                                                                                                 | `devbench/src/Tools.cpp:1534`                                                           |
| `sceneMismatch` produced as a **step-local** result field only                                                | `devbench/src/Tools.cpp:1738-1767`                                                      |
| `waitFor` poll loop idiom (`HeadSeq` → `Since` → sleep → deadline)                                            | `devbench/src/Tools.cpp:1626-1643`                                                      |
| `EventBus::Publish` is mutex-guarded, returns immediately, delivers off-thread                                | `devbench/src/EventBus.h:97-113, 136-160`; ring bound `kMaxRecent = 256` at `:133`      |
| `BuildReplaySteps`: compat gate, restore prologue, scene assert, trajectory copy + `coc/cow` settle injection | `devbench/src/Recording.cpp:581-592, 602-663, 669-683, 690-701`                         |
| `BuildScenario` emits `{"pose":…,"wait":Δ}` where Δ is the recorder's inter-sample `tMs` delta                | `devbench/src/Recording.cpp:203-205, 237-268`                                           |
| `directory_iterator` precedent                                                                                | `devbench/src/Recording.cpp:503, 770`; `Tools.cpp:185`                                  |
| INI read precedent                                                                                            | `devbench/src/Tools.cpp:165-170`                                                        |
| `RE::MenuControls::QueueScreenshot()` — null-checks the handler, returns `false` if already queued            | `devbench/lib/commonlibsse-ng/src/RE/M/MenuControls.cpp:57-64`                          |
| devbench version 1.13.0 → build 11300                                                                         | `devbench/xmake.lua:18`                                                                 |
| OS `openshaders.capture kind=screenshot` is fire-and-forget, no path, no completion                           | `open-shaders/src/Features/RemoteControl/DevBenchBridge.cpp:751-761`                    |
| OS screenshot filename generated internally; save on a background worker; HUD toast on save                   | `open-shaders/src/Features/ScreenshotFeature.cpp:435-447, 865-925, 1016`                |
| `ResolveToAbsoluteGamePath` = `GetModuleFileNameW` parent (game root)                                         | `open-shaders/src/Features/ScreenshotFeature.cpp:203-216`                               |

**Two corrections to premises that were handed to the design pass, both verified against source:**

1. **`waitUntil noMenu` is unusable during gameplay.** `CheckState("noMenu")` is `GetOpenMenus().empty()` (`Tools.cpp:1397`), and `HUD Menu` is permanently in the tracked set — it is explicitly filtered out at `Tools.cpp:1332`. A `waitUntil noMenu` step before a capture would burn its entire timeout every time. **Use `waitUntil noBlockingMenu`.**
2. **`menu action=close` requires a `name`** (`Tools.cpp:485-487`). A blind "close whatever is open" step is not expressible; the macro uses `assert noBlockingMenu` + `waitUntil noBlockingMenu` instead.

---

## Part 0 — Prerequisite: registrant list (`inspect kind=registrants`)

Closes `ROADMAP.md:298-306`. Ships standalone.

**Files:** `src/HostApi.h`, `src/HostApi.cpp`, `src/Tools.cpp`.

`OnInterfaceRequest` (`HostApi.cpp:157-163`) already receives `a_message->sender`. Add a mutex-guarded ledger:

```cpp
// HostApi.h
namespace dvb::HostApi
{
    struct Consumer {
        std::string   name;        ///< a_message->sender, or "<?>"
        unsigned int  revision;    ///< a_revisionNumber passed to GetApi
        long long     atEpoch;
        std::uint32_t atFrame;
    };
    struct Registration {
        std::string   kind;        ///< "tool" | "extension"
        std::string   name;        ///< tool name, or "<baseTool>:<key>"
        long long     atEpoch;
        std::uint32_t atFrame;
        bool          replaced;    ///< Register returned false
    };
    std::vector<Consumer>     Consumers();
    std::vector<Registration> Registrations();
}
```

- `OnInterfaceRequest` appends a `Consumer` before setting `GetApiFunction`.
- `Interface::RegisterTool` (`HostApi.cpp:47`) and `Interface::RegisterToolExtension` (`HostApi.cpp:76`) each append a `Registration`.
- Both lists guarded by one `std::mutex`; accessors return copies.

**No cross-attribution.** The interface is a shared singleton with no caller identity (`ROADMAP.md:302-306`); "the last requester owns this registration" would be a confident lie. The two lists are returned side by side.

New built-in inspect kind, inserted in `InspectHandler` immediately before the `extensions` branch at `Tools.cpp:1100`:

```jsonc
// inspect kind=registrants →
{
  "consumers": [
    {
      "name": "CommunityShaders",
      "revision": 1,
      "atEpoch": 1765412300,
      "atFrame": 41,
    },
  ],
  "registrations": [
    {
      "kind": "tool",
      "name": "openshaders.capture",
      "atEpoch": 1765412301,
      "atFrame": 42,
      "replaced": false,
    },
    {
      "kind": "extension",
      "name": "capture:openshaders",
      "atEpoch": 1765412301,
      "atFrame": 42,
      "replaced": false,
    },
  ],
  "capabilities": {
    "capture": ["openshaders"],
    "inspect": ["openshaders", "profiler", "shadercache", "llfshadows"],
    "menu": ["CommunityShaders"],
  },
}
```

`capabilities` is built from `ToolExtensions::Keys(base)` for `capture|inspect|menu`. This is the exact data the Part 2b capability gate consumes; `consumers`/`registrations` exist for humans and error text.

Also: add `"registrants"` to the kinds enum at `Tools.cpp:1870`, to the description at `:1867`, and to the unknown-kind message at `:1116`.

---

## Part 1 — devbench: `capture`, a third extension-enabled base tool

**Files:** new `src/Capture.h`, `src/Capture.cpp`; edits to `src/Tools.cpp`, `src/Config.h`, `src/Config.cpp`, `src/main.cpp`, `include/DevBenchAPI.h` (documentation only).

### 1a. Registration and dispatch

`capture` is a top-level tool, kind-dispatched exactly like `inspect`, opted into `ToolExtensions`. Registered next to `Tools.cpp:2002-2003`:

```cpp
a_registry.Register(BuildCaptureDescriptor(), &Capture::Handle);
```

The change-listener at `Tools.cpp:2009-2016` gains a third arm:

```cpp
else if (base == "capture")
    reg->Register(BuildCaptureDescriptor(), &Capture::Handle);
```

Built-in kinds: `native`, `providers`, `extensions`. Any other kind routes through `ToolExtensions::Find("capture", kind)`, mirroring `Tools.cpp:1113`.

### 1b. Kind resolution — `auto` never resolves to native

This is a hard rule, applied at **every** entry point (checkpoint macro, bare tool call, MCP/REST client):

| `kind`     | `allowNative` | Outcome                                                                                            |
| ---------- | ------------- | -------------------------------------------------------------------------------------------------- |
| `"auto"`   | absent/false  | exactly one registered provider ⇒ that provider; **zero ⇒ 404**; **two or more ⇒ 400** naming them |
| `"auto"`   | `true`        | as above, but zero providers ⇒ native                                                              |
| `"native"` | —             | native, always                                                                                     |
| `"<key>"`  | —             | that provider; unregistered ⇒ 404                                                                  |

404 message: `"no capture provider registered; install one (see inspect kind=registrants), or pass kind='native' / allowNative:true for the low-fidelity vanilla fallback (NOT comparable against provider-authored goldens)"`.

`allowNative` is supplied by the checkpoint macro **only** from a recording's declared `meta.capabilities[].allowNative` (Part 2a), and defaults to `false` everywhere. Rationale: a recording with no `capabilities` block — i.e. every recording that exists today — must not silently produce an INI-format vanilla BMP that a scorer then SSIMs against a provider-authored PNG. A missing provider is a loud, fixable 404.

### 1c. Tool input schema

```jsonc
{
  "kind": "auto (default) | native | providers | extensions | <registered provider key>",
  "checkpointId": "string, REQUIRED — stable file stem and correlation key",
  "recording": "string — recording file stem (correlation)",
  "variant": "string — variant under test, default \"default\" (correlation)",
  "allowNative": "boolean, default false — permit kind=auto to fall back to the vanilla path",
  "excludeUi": "boolean, default true — request a pre-UI source; native cannot honor it",
  "outDir": "string — override the capture bundle dir (absolute, or relative to game root)",
  "timeoutMs": "integer, default config.captureTimeoutMs (8000)",
  "pollMs": "integer, default 100",
  "subrect": "object {x,y,w,h} in 0..1 UV — OPTIONAL, provider-only; native ignores it",
  "cleanup": "boolean, default false — native only: delete the game's source file after copying",
  "runId": "integer — injected by ScenarioHandler (correlation)",
  "repeat": "integer — injected by ScenarioHandler when repeat > 1 (filename suffix)",
  "atMs": "integer — injected by the checkpoint macro (authored anchor)",
  "resolvedAtMs": "integer — injected by the checkpoint macro (actual anchor)",
  "resolvedIndex": "integer — injected by the checkpoint macro (index in the assembled step list)",
}
```

### 1d. Tool output — identical shape for every provider

```jsonc
{
  "ok": true,
  "provider": "openshaders", // or "native"
  "kind": "screenshot",
  "path": "E:/…/Data/SKSE/Plugins/devbench/captures/GuardianStonesToWhiterun/variantA/01_stones_north.png",
  "file": "01_stones_north.png",
  "bytes": 8123456,
  "width": 2560,
  "height": 1440,

  "checkpointId": "01_stones_north",
  "recording": "GuardianStonesToWhiterun",
  "variant": "variantA",
  "runId": 42,
  "repeat": null,
  "atMs": 12000,
  "resolvedAtMs": 12003,
  "resolvedIndex": 1207,

  "frame": 918273,
  "epoch": 1765412345,
  "worldspaceFormID": 60,
  "cellFormID": 39825,
  "gameHour": 14.36,
  "weatherFormID": 2074,

  "sceneMismatch": false,
  "uiExcluded": true,
  "readyBy": "event", // "event" | "poll"
  "elapsedMs": 412,
  "requestId": "01_stones_north#42#17",
  "degraded": [], // e.g. ["nativeFallback","formatFromIni","hudNotSuppressed",
  //       "sceneMismatch","uiIncluded","cleanupFailed"]
  "sourcePath": "E:/…/ScreenShot42.bmp", // native only
}
```

devbench additionally:

- writes a sidecar `<stem>.json` next to the image containing this exact object — so the Python scorer can walk the captures tree standalone (golden refresh, CI artifact upload) without replaying the HTTP transcript;
- publishes `capture.saved` with the same object on the EventBus;
- publishes `capture.abandoned` `{requestId, path, checkpointId, runId, reason}` on timeout, so the harness can distinguish "provider never finished" from "provider wrote a bad image".

**Output paths.** `<captureDir>/<recording>/<variant>/<checkpointId>[__r<rep>].<ext>`, overwritten each run (deterministic ⇒ trivially scriptable). The `__r<rep>` suffix is appended **only** when the injected `repeat` field is present, so a `repeat`-ed replay produces N distinct artifacts per checkpoint instead of one silently overwritten file. Golden/candidate versioning is entirely a Python concern.

### 1e. Header

```cpp
// src/Capture.h
namespace dvb::Capture
{
    json Handle(const json& a_args, const ToolContext& a_ctx);   ///< the `capture` tool handler
    json Native(const json& a_args);                             ///< vanilla fallback (1g)
    json ListScreenshots(const json& a_args);                    ///< inspect kind=screenshots (1h)

    std::filesystem::path GameRoot();                            ///< GetModuleFileNameW parent
    std::filesystem::path CaptureDir(const json& a_args);        ///< <captureDir>/<recording>/<variant>

    /// Wait for a provider artifact. Satisfied by a `capture.ready` event whose requestId
    /// matches EXACTLY, or by the file existing, being writer-released, and size-stable.
    struct Ready { bool ok; std::string readyBy; std::string error; std::uintmax_t bytes; };
    Ready AwaitArtifact(EventBus& a_events, std::uint64_t a_sinceSeq,
                        const std::string& a_requestId, const std::filesystem::path& a_path,
                        long a_timeoutMs, long a_pollMs);

    void SetEvents(EventBus* a_events);        ///< wired from RegisterCoreTools
    void SetDefaults(const Config& a_cfg);     ///< wired from main.cpp at kPostLoad
}
```

`AwaitArtifact` reuses the exact loop shape of `Tools.cpp:1626-1643` (`Since(since)` → advance `since` → sleep `pollMs` → deadline).

### 1f. Provider contract

Documented in the `capture` descriptor and above `RegisterToolExtension` in `include/DevBenchAPI.h:76-88`.

A provider registered under base tool `capture` receives the full args object, which devbench guarantees additionally contains:

- `outputPath` — absolute, forward-slash; **devbench has already created the parent directory**;
- `requestId` — opaque, globally unique, format `<checkpointId>#<runId>#<atomic seq>` where the counter is a process-wide `std::atomic<uint64_t>`. Uniqueness is load-bearing: a late event from a timed-out checkpoint must never match a later wait.

The provider must:

1. return synchronously `{"queued":true,"requestId":…,"path":outputPath}` (or `{"ok":true,"path":…}` if it completed inline);
2. write **exactly** that path, PNG, lossless;
3. honor `excludeUi` if it can, and report `uiExcluded` in its return;
4. emit `capture.ready` `{"requestId","path","ok","error"?,"uiExcluded"?,"width"?,"height"?}` once the bytes are on disk.

Returning `{"error":…}` fails the call immediately with **502**.

**Ordering requirement, load-bearing:** `sinceSeq = a_events.HeadSeq()` is snapshotted **immediately before** invoking the provider handler, never after. Inverted, a fast provider could publish before the snapshot and the wait would miss the event and burn its full timeout (a false 504).

If a provider never emits, the file-readiness poll still resolves it and the result is stamped `readyBy:"poll"` — a working-but-degraded provider is visible, not silent. The ring is bounded at 256 events (`EventBus.h:133`); during a dense replay `scenario.step` is published only every 100th step (`Tools.cpp:1551`), so eviction inside an 8s window is unlikely, and the poll covers it if it happens. Documented, not further defended.

### 1g. Native fallback (`kind:"native"`)

Rock-bottom smoke-test path. No D3D11, no image codec — a main-thread flag flip plus a directory poll, the same complexity class as the existing main-thread work in `Tools.cpp`.

**Step 1 — preflight (main thread, `MainThread::RunAndWait`, ≤1000ms, idiom per `Tools.cpp:1310`).** Read `RE::INISettingCollection::GetSingleton()->GetSetting("bAllowScreenShot:Display")` (INI precedent `Tools.cpp:165-170`). Present and false ⇒ **409** `"vanilla screenshots disabled (bAllowScreenShot:Display=0)"`. Absent ⇒ proceed.

**Step 2 — snapshot the scan dirs.** `directory_iterator` over `config.captureScanDirs` (default `["", "Screenshots"]`, resolved against `GameRoot()`), non-recursive, extensions `{.bmp,.png,.jpg,.jpeg,.dds}`. Record **per file**: path, size, `last_write_time`.

**Step 3 — queue (main thread).** `RE::MenuControls::GetSingleton()->QueueScreenshot()`. Returns `false` ⇒ **409** `"no ScreenshotHandler, or a shot is already queued"`.

**Step 4 — poll for a completed candidate**, every `pollMs` (default 100) until `timeoutMs` (default 8000).

Three-way candidate rule — mtime is compared **per file against its own snapshot value**, never against a wall-clock `t0` (that would be a clock-domain error):

> candidate ⇔ `path ∉ snapshot` **OR** `size(path) != snapshot[path].size` **OR** `mtime(path) > snapshot[path].mtime`

The set-difference branch is the primary path and is completely independent of NTFS timestamp granularity or caching — the engine writes a **new** auto-incremented filename every time and never overwrites in place. The other two branches are defence in depth.

Completeness is decided by an exclusive-open probe, not by a size heuristic. Magic-byte checks do not prove completeness (the header is written first):

```cpp
// src/Capture.cpp — file-local. True only when no other handle is open on the file.
bool IsWriterDone(const std::filesystem::path& a_p)
{
    HANDLE h = CreateFileW(a_p.c_str(), GENERIC_READ, 0 /*no sharing*/, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;                    // ERROR_SHARING_VIOLATION → the writer still holds it
    CloseHandle(h);
    return true;
}
```

A candidate is accepted only when `size > 0 && IsWriterDone(p) && size unchanged since the previous poll`. Timeout ⇒ **504** naming every directory scanned.

**Step 5 — copy into the bundle,** with bounded retry against AV / search-indexer / cloud-sync locks. The `std::error_code` overloads are mandatory (they do not throw; and `ScenarioHandler:1774-1791` would contain a throw anyway, but silent non-throwing failure is the wrong outcome here):

```cpp
bool CopyWithRetry(const std::filesystem::path& a_from, const std::filesystem::path& a_to,
                   std::string& a_err)
{
    for (int i = 0; i < 5; ++i) {
        std::error_code ec;
        std::filesystem::copy_file(a_from, a_to,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec)
            return true;
        a_err = ec.message();
        std::this_thread::sleep_for(std::chrono::milliseconds(50 * (1 << i)));  // 50…800ms
    }
    return false;
}
```

`cleanup:true` runs `fs::remove` through the same retry shape; a failure there is downgraded to `degraded:["cleanupFailed"]`, never a step failure — a leftover source file is cosmetic. `cleanup` defaults to **false**: non-destructive, and safe because step 2 snapshots per call, so leftovers can never be mistaken for a later capture.

**Step 6 — report.** Always `uiExcluded:false`; `degraded` always contains `["nativeFallback","formatFromIni","hudNotSuppressed"]`. Write the sidecar, publish `capture.saved`.

**Assumptions, explicitly labelled (none verified against a running game in this pass):**

- **A1** — vanilla SE writes into the game root (`GetModuleFileNameW` parent). Supported by convention plus Open Shaders' own `ResolveToAbsoluteGamePath` (`ScreenshotFeature.cpp:203-216`). Mitigated by the configurable dir list and by Part 1h letting a human discover the real dir in one call.
- **A2** — ENB/ReShade hook the screenshot _key_, not the engine's queued-screenshot path, so `QueueScreenshot()` reaches the engine writer. If false, the poll 504s cleanly rather than misbehaving.
- **A3** — the vanilla index counter is in-memory, so `cleanup:true` cannot cause a filename collision. This is why `cleanup` defaults to `false`.
- **A4** — `bAllowScreenShot` lives under `Display` with that exact key. Guarded: absent ⇒ proceed.

### 1h. `inspect kind=screenshots`

Same snapshot helper as 1g step 2, exposed independently. Added to `InspectHandler` before `Tools.cpp:1100`, to the enum at `:1870`, to the description at `:1867`.

```jsonc
// inspect kind=screenshots [dir?] [limit? default 50] →
{
  "dirs": [
    "E:/…/Skyrim Special Edition",
    "E:/…/Skyrim Special Edition/Screenshots",
  ],
  "count": 12,
  "returned": 12,
  "truncated": false,
  "screenshots": [
    {
      "file": "ScreenShot42.bmp",
      "path": "E:/…/ScreenShot42.bmp",
      "bytes": 12441654,
      "mtimeEpoch": 1765412345,
    },
  ],
}
```

Newest first (matches `EnumerateSaves`, `Tools.cpp:189`); `{count, returned, truncated}` per the convention at `Tools.cpp:347`.

### 1i. Config additions

```cpp
// src/Config.h
std::string              captureDir      = "Data/SKSE/Plugins/devbench/captures";
std::vector<std::string> captureScanDirs = { "", "Screenshots" };   // relative to game root
int                      captureTimeoutMs = 8000;
int                      captureSettleMs  = 500;   // default settle before each checkpoint
```

Wired in `main.cpp` alongside `Recording::SetLoadSettleMs(cfg.loadSettleMs)` via `Capture::SetDefaults(cfg)` and `Recording::SetCaptureDefaults(cfg.captureSettleMs)`.

### 1j. Run-scoped injection in `ScenarioHandler`

`sceneMismatch` is a step-local result field (`Tools.cpp:1753`); nothing carries it forward. The only place run-scoped state can be injected is the tool branch. Three surgical edits:

```cpp
// Tools.cpp:1534 — declare INSIDE the repeat loop: rep N's scene verdict is rep N's alone.
for (int rep = 0; rep < repeat && !aborted; ++rep) {
    bool runSceneMismatch = false;
    for (size_t i = 0; i < steps.size() && !aborted; ++i) {
        …
```

```cpp
// Tools.cpp:1669-1678 — `args` becomes NON-CONST so run-scoped context can be injected.
} else if (step.contains("tool")) {
    const std::string tool = step["tool"].get<std::string>();
    json              args = step.value("args", json::object());   // was: const json args
    r["kind"] = "tool";
    r["tool"] = tool;
    if (step.contains("label"))
        r["label"] = step["label"];
    if (tool == "capture") {                       // context the static step list cannot carry
        args["runId"] = runId;
        if (repeat > 1)      args["repeat"] = rep;         // → "__r<rep>" filename suffix
        if (runSceneMismatch) args["sceneMismatch"] = true;
    }
    ToolContext stepCtx = a_ctx;
    stepCtx.internal = true;
    const ToolResult tr = a_registry.Invoke(tool, args, stepCtx);
    …
```

```cpp
// Tools.cpp:1738-1746 (assert scene, !ready) AND :1747-1762 (assert scene, mismatch):
runSceneMismatch = true;
```

An unconfirmed scene is set too, not just a mismatched one — a shot taken while the scene never finished loading is equally unusable as a golden reference.

The injected fields reach the transcript **via the result**, not via the args: `Capture::Handle` echoes `runId`, `repeat`, `checkpointId`, `recording`, `variant`, `sceneMismatch`, `atMs`, `resolvedAtMs`, `resolvedIndex` into its output, and `ScenarioHandler` stores that at `r["result"] = tr.value` (`Tools.cpp:1681`). That is the right place — the result also records what the tool actually _resolved_ (`provider`, `readyBy`, `uiExcluded`, `degraded`), which args could never show. The on-disk sidecar is a second, independent copy.

When `sceneMismatch` is true the capture **still runs** (a mismatched shot is diagnostic evidence) but is stamped `degraded:["sceneMismatch"]` so the Python scorer marks it inconclusive rather than reporting a false regression.

---

## Part 2 — devbench: checkpoints in the recording manifest

**Files:** `src/Recording.cpp` (`BuildReplaySteps`, 491-714), `src/Recording.h` (doc comment), `src/Tools.cpp` (`record` schema at 2164-2177).

### 2a. Manifest schema (purely additive; every existing recording stays valid)

```jsonc
"meta": {
  "capabilities": [
    { "capability": "capture", "provider": "openshaders", "required": true, "allowNative": false }
  ],
  "checkpoints": [
    { "id": "01_stones_north", "atMs": 12000, "pov": "first", "settleMs": 750,
      "excludeUi": true,
      "subrect": {"x":0.25,"y":0.25,"w":0.5,"h":0.5},
      "note": "guardian stones, sun behind camera" }
  ]
}
```

- `capabilities[].provider` omitted ⇒ any registered `capture` provider satisfies the gate.
- `capabilities[].allowNative` defaults to `false` — the **only** source of `allowNative` anywhere in the system.
- Unknown `capability` values are ignored by the gate (forward-compatible).
- `subrect` is optional and provider-only; ROI is a Python concern by default (Part 4).

**`atMs` is the only anchor.** Justification, derived from source rather than asserted:

1. `BuildScenario` (`Recording.cpp:237-268`) emits one `{"pose":…,"wait":Δ}` or `{"wait":Δ}` per sample, where `Δ = tMs[k] − tMs[k−1]` and `tMs` is the recorder's wall-clock offset stamped at sample time (`Recording.cpp:203-205, 259-260`). Therefore the sum of `wait` over the first _k_ trajectory steps **is** the recorder's `tMs` at sample _k_ — the same numbers, only clamped to `max(1, Δ)`.
2. The insertion loop accumulates `cumMs` **at plan time**, inside `BuildReplaySteps`, as pure arithmetic over a static JSON array. No clock is read.
3. Therefore `atMs → index` is resolved deterministically _before the run starts_. Nothing at execution time — a slow cell load, a 40s `waitFor postLoadGame`, frame-rate variance, `RunAndWait` latency inflating real step duration above `wait` — can move it. Variable-duration steps shift the checkpoint's _wall-clock_ firing time only; its position relative to the surrounding pose steps is invariant, and that is the property that matters: the shot must follow the correct `player.setpos`/`setangle`, not a stopwatch reading.

A step index would be _less_ robust: it breaks the moment anyone hand-edits or re-serializes a recording, which the format explicitly invites (`Recording.cpp:285-287`: "one compact step per line … hand-editable and git-diffable"). `atMs` is stable under insertion/deletion of neighbouring samples.

### 2b. Capability gate

Inserted immediately after the runtime/compat gate at `Recording.cpp:581-592` — same shape, same `force` downgrade, same 409, deliberately not a second failure idiom:

```cpp
const json captureCap = FindCaptureCapability(meta);          // the meta.capabilities entry, or {}
if (!captureCap.empty() && captureCap.value("required", true)) {
    const std::string want = captureCap.value("provider", std::string{});
    const auto        keys = ToolExtensions::Keys("capture");
    bool ok = want.empty() ? !keys.empty()
                           : ToolExtensions::Find("capture", want).has_value();
    if (!ok && captureCap.value("allowNative", false))
        ok = true;                                            // native is always available
    if (!ok && !force)
        throw ToolError(409, std::format(
            "recording requires capture provider '{}' but none is registered (registered: [{}]) — "
            "install the provider mod (see inspect kind=registrants), set "
            "meta.capabilities[].allowNative, or pass force",
            want.empty() ? "<any>" : want, JoinNames(keys)));
}
```

### 2c. Checkpoint expansion — a macro, not a new step kind

`ROADMAP.md:311-318` forbids growing `scenario` into a DSL. A checkpoint is therefore expanded by `BuildReplaySteps` into existing primitives; `ScenarioHandler` gains **no** new step type (only the run-scoped injection of 1j).

Rewrite the trajectory copy loop (`Recording.cpp:690-701`):

```cpp
long   cumMs = 0;
size_t cpIdx = 0;
const json checkpoints = SortedCheckpoints(meta);   // validates, sorts by atMs

for (const auto& s : rec["steps"]) {
    steps.push_back(s);
    // LOAD-BEARING: accumulate ONLY over rec["steps"] — the recorder's own clock.
    // The coc/cow settle steps injected below, and the restore-prologue waits at
    // Recording.cpp:602-663, are NOT part of that clock and must never be summed.
    if (s.contains("wait"))
        cumMs += s["wait"].get<long>();

    /* existing coc/cow settle injection, Recording.cpp:692-700 — unchanged, NOT summed */

    while (cpIdx < checkpoints.size() && checkpoints[cpIdx].value("atMs", 0L) <= cumMs)
        AppendCheckpointSteps(steps, checkpoints[cpIdx++], meta, captureCap, a_args, cumMs);
}
// Checkpoints anchored past the end of the trajectory still fire.
while (cpIdx < checkpoints.size())
    AppendCheckpointSteps(steps, checkpoints[cpIdx++], meta, captureCap, a_args, cumMs);
```

```cpp
// Recording.cpp, file-local
json SortedCheckpoints(const json& a_meta);   // ToolError(400) on duplicate id, missing id,
                                              // or negative atMs; warns when atMs exceeds the
                                              // total trajectory duration (still fires, end-flush)
void AppendCheckpointSteps(json& a_steps, const json& a_cp, const json& a_meta,
                           const json& a_cap, const json& a_args, long a_cumMs);
```

`AppendCheckpointSteps` emits exactly:

```jsonc
{"assert":"noBlockingMenu"}                                       // reuses Tools.cpp:1691
{"waitUntil":"noBlockingMenu","timeoutMs":5000,"pollMs":100}      // NOT noMenu — HUD Menu is always open
{"tool":"camera","args":{"action":"setPov","pov":"first"}}        // only when cp.pov present
{"wait": <cp.settleMs | config.captureSettleMs>}
{"tool":"capture","args":{ …see below… }}
```

```cpp
json capArgs{
    { "kind",          a_cap.value("provider", std::string("auto")) },  // NAMED provider, not "auto"
    { "allowNative",   a_cap.value("allowNative", false) },
    { "checkpointId",  a_cp.at("id") },
    { "recording",     recordingStem },
    { "variant",       a_args.value("variant", std::string("default")) },
    { "excludeUi",     a_cp.value("excludeUi", true) },
    { "atMs",          a_cp.value("atMs", 0L) },
    { "resolvedAtMs",  a_cumMs },
    { "resolvedIndex", static_cast<long>(a_steps.size()) },
};
if (a_cp.contains("settleMs")) capArgs["timeoutMs"] = /* config.captureTimeoutMs */;
if (a_cp.contains("subrect"))  capArgs["subrect"]   = a_cp["subrect"];
a_steps.push_back(json{ { "tool", "capture" }, { "args", std::move(capArgs) } });
```

Design notes, each traced to code:

- **`kind` is the resolved provider, never `"auto"`.** The macro and the 2b gate read the _same_ `captureCap` object, so they cannot disagree by construction. Emitting `"auto"` while the gate validated a named provider would 400 mid-replay whenever a second provider happened to be installed — exactly the silent-late-failure the design exists to prevent.
- **`waitUntil noBlockingMenu`**, not `noMenu`. `BlockingMenus()` (`Tools.cpp:1307-1338`) already excludes HUD/Cursor/Console and already survives a paused-frame `RunAndWait` 504.
- **No pose step is emitted.** The trajectory's own `pose` step immediately preceding the checkpoint already set x/y/z/yaw/pitch (`Tools.cpp:1566-1610`); re-issuing it is a no-op costing five console round-trips. Only `pov` is re-asserted, because Skyrim's idle-vanity timer can flip it — the exact hazard called out at `Tools.cpp:1197`.
- **No `tm`.** HUD suppression is _not_ attempted from the step list. `tm` is a blind toggle: it un-hides an already-hidden HUD, and the balanced un-hide leaks whenever a step fails (`aborted` is set at `Tools.cpp:1799` and the loop exits before the trailing step runs). UI exclusion moves into the capture contract as `excludeUi`, where the provider — which actually owns the render surface — decides. Native reports `uiExcluded:false` and `degraded:["hudNotSuppressed"]`, which is honest and consistent with native already being excluded from golden comparison by 1b.
- `resolvedAtMs`/`resolvedIndex` make the anchoring falsifiable at runtime rather than arguable. `resolvedAtMs ≥ atMs` by at most one sample interval.

**Test to write alongside this:** a unit/integration test asserting that a recording whose trajectory contains a captured `coc` produces the _same_ `resolvedIndex`-relative pose neighbour as the same recording without one — i.e. that the injected settle wait was not summed into `cumMs`.

### 2d. `record` tool

New arg `variant` (string, default `"default"`), threaded into the emitted capture steps; added to the input schema at `Tools.cpp:2164-2177` and to the description at `:2137-2163`.

---

## Part 3 — Open Shaders: reference capture provider

**Files:** `src/Features/ScreenshotFeature.h`, `src/Features/ScreenshotFeature.cpp`, `src/Features/RemoteControl/DevBenchBridge.cpp`.

### 3a. `ScreenshotFeature` — targeted capture with a completion signal

Today `Capture()` computes its own timestamped name (`:1016` → `BuildScreenshotPath`, `:435-447`) and the worker (`:865-925`) signals nothing.

```cpp
// ScreenshotFeature.h — public
struct CaptureRequest
{
    std::filesystem::path outputPath;    ///< empty ⇒ legacy BuildScreenshotPath behaviour
    std::string           requestId;     ///< empty ⇒ no completion callback
    bool                  forcePng   = true;   ///< override sdrUsePng for this shot only
    bool                  useSubrect = false;  ///< override applyCropToScreenshot for this shot only
    bool                  excludeUi  = true;   ///< prefer a pre-UI source
    bool                  quiet      = false;  ///< suppress the in-game "saved" toast
    std::optional<Util::Subrect::Rect> subrect;
};

/// THE single entry point for every capture — hotkey, ImGui button, and devbench provider
/// calls all funnel through here, so the queue is the one source of truth. Thread-safe.
void RequestCapture(CaptureRequest a_req);

using CompletionFn = void (*)(const char* a_requestId, const char* a_path,
                              bool a_ok, bool a_uiExcluded, const char* a_error);
static void SetCompletionCallback(CompletionFn a_fn);   ///< atomic; nullptr disables
```

Private additions: `std::mutex requestMutex; std::deque<CaptureRequest> pendingRequests;`, and `std::string requestId; bool quiet; bool uiExcluded;` on `PendingScreenshot` (`ScreenshotFeature.h:54-65`).

```cpp
void ScreenshotFeature::RequestCapture(CaptureRequest a_req)
{
    {
        std::lock_guard<std::mutex> lock(requestMutex);
        pendingRequests.push_back(std::move(a_req));
    }
    captureRequested.store(true, std::memory_order_release);
}

// Replaces ScreenshotFeature.cpp:826-831.
void ScreenshotFeature::ProcessCaptureRequest()
{
    if (!captureRequested.exchange(false))
        return;
    Capture();                       // services at most ONE request — one backbuffer per frame
    {
        std::lock_guard<std::mutex> lock(requestMutex);
        if (!pendingRequests.empty())
            captureRequested.store(true, std::memory_order_release);   // re-arm for next frame
    }
}
```

Deliberately **not** looping `Capture()` within one frame: two captures of the same frame would copy an identical backbuffer and emit two byte-identical PNGs under different checkpoint ids — worse than a one-frame delay.

Existing callers change to funnel through the queue:

```cpp
// ScreenshotFeature.cpp:703 (ImGui button) and the hotkey path:
RequestCapture({});      // empty outputPath/requestId ⇒ legacy behaviour, FIFO with devbench requests
```

Without this, a hotkey press setting the bare flag while a devbench request sits in the queue makes `Capture()` service the devbench request and silently consume the hotkey's intent. With the queue as sole source of truth, N requests ⇒ N shots on N successive frames, FIFO, none dropped or misattributed; `captureRequested` degenerates into a cheap "queue non-empty" hint readable without taking the mutex every frame.

`Capture()` (`:935-1017`) changes:

- pop one `CaptureRequest` under `requestMutex` at entry; empty queue ⇒ default-constructed request ⇒ legacy path preserved exactly;
- crop block `:961-966` honors `req.useSubrect` / `req.subrect` instead of the member `applyCropToScreenshot`;
- `:1008` becomes `saveAsSdrPng = !saveAsHdrPng && (req.forcePng || sdrUsePng)`;
- `:1016` becomes `screenshot.outputPath = req.outputPath.empty() ? BuildScreenshotPath(...) : req.outputPath`;
- carry `req.requestId`, `req.quiet`, and the actual UI-exclusion outcome of `SelectCaptureSource` into `PendingScreenshot`.

`ScreenshotWorkerLoop()` (`:865-925`):

- after `SaveScreenshotToDisk`, if `requestId` is non-empty, load the completion callback into a local `CompletionFn`, null-check, and invoke `(requestId, outputPath, saveOk, uiExcluded, error)`;
- **also invoke it with `ok=false` on the early-`continue` map failure at `:889-893`** — otherwise a mapping failure hangs devbench for the full timeout instead of failing in about one frame;
- **suppress the HUD toast** (`:920-921`) when `quiet` is set: a "Screenshot saved" message lands in the _next_ checkpoint's frame and is itself a visual diff.

```cpp
// ScreenshotFeature.cpp — clear the callback BEFORE joining, so nothing can be emitted
// once teardown has begun.
void ScreenshotFeature::StopWorkerThread()
{
    SetCompletionCallback(nullptr);
    /* existing: screenshotWorkerRunning = false; notify_all; join */
}
```

`~ScreenshotFeature` already calls `StopWorkerThread` (`ScreenshotFeature.h:16`). SKSE never unloads plugins mid-session and devbench's `g_server` (`main.cpp:21`) is never reset, so the only remaining hazard is cross-DLL static-destruction order at process exit; clearing the callback first closes the common case, and devbench never treats a missing event as fatal (the poll resolves it). Residual process-exit risk: accepted and documented.

### 3b. `DevBenchBridge` — register the contract

At `DevBenchBridge.cpp:1056` (beside the existing `GetBuildNumber() >= 10500` block), add:

```cpp
if (dvb->GetBuildNumber() >= 11400) {
    g_dvb.store(dvb, std::memory_order_release);            // namespace-scope std::atomic<…*>
    ScreenshotFeature::SetCompletionCallback(&OnCaptureComplete);
    dvb->RegisterToolExtension("capture", "openshaders", captureProviderDesc,
                               &CaptureProviderHandler, nullptr);
} else {
    logger::info("DevBenchBridge: devbench build {} < 11400; capture provider needs 1.14.0",
                 dvb->GetBuildNumber());
}
```

```cpp
// anonymous namespace — captureless free function, runs on the ENCODE WORKER thread.
// Safe: HostApi::Interface::EmitEvent (HostApi.cpp:93-105) only reaches EventBus::Publish,
// which is mutex-guarded and delivers off-thread (EventBus.h:97-113).
void OnCaptureComplete(const char* a_requestId, const char* a_path, bool a_ok,
                       bool a_uiExcluded, const char* a_error)
{
    auto* dvb = g_dvb.load(std::memory_order_acquire);
    if (!dvb || !a_requestId || !*a_requestId)
        return;
    json p{ { "requestId", a_requestId }, { "path", a_path ? a_path : "" },
            { "ok", a_ok }, { "uiExcluded", a_uiExcluded } };
    if (!a_ok && a_error && *a_error)
        p["error"] = a_error;
    dvb->EmitEvent("capture.ready", p.dump().c_str());
}

json BuildCaptureProviderResult(const json& a_args)
{
    const std::string out = a_args.value("outputPath", std::string{});
    const std::string rid = a_args.value("requestId",  std::string{});
    if (out.empty() || rid.empty())
        return json{ { "error", "capture provider contract requires outputPath and requestId" } };

    return RunOnMainThread([out, rid,
                            sub = a_args.value("subrect", json::object()),
                            excludeUi = a_args.value("excludeUi", true)]() -> json {
        auto* shot = &globals::features::screenshotFeature;
        if (!shot->loaded)
            return json{ { "error", "Screenshot feature is not loaded" } };
        ScreenshotFeature::CaptureRequest req;
        req.outputPath = std::filesystem::path(out);
        req.requestId  = rid;
        req.forcePng   = true;
        req.excludeUi  = excludeUi;
        req.quiet      = true;
        if (!sub.empty()) { req.useSubrect = true; req.subrect = ParseSubrect(sub); }
        shot->RequestCapture(std::move(req));
        return json{ { "queued", true }, { "requestId", rid }, { "path", out },
                     { "enqueued_at_frame", EnqueuedFrame() } };
    });
}

void CaptureProviderHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
{
    RunHandler(&BuildCaptureProviderResult, a_argsJson, a_sink, a_write);
}
```

`RunOnMainThread` is required for the same reason `DevBenchBridge.cpp:752-760` uses it (`loaded` is mutated by queued toggle tasks).

The existing top-level `openshaders.capture` tool (`:1043`) is left untouched for `renderdoc` / `shadowmaps` / interactive use; its `kind=screenshot` description gains one sentence pointing at the `capture` extension for path-controlled captures.

**Assumption A5:** `SelectCaptureSource` (`ScreenshotFeature.cpp:944`) yields a post-tonemap, pre-ImGui surface, so the CS settings window is not composited into a provider capture. Unverified in this pass. If false, the provider reports `uiExcluded:false` (devbench then stamps `degraded:["uiIncluded"]`) and the checkpoint macro gains a `{"tool":"menu","args":{"action":"invoke","name":"CommunityShaders","op":"close"}}` step.

---

## Part 4 — Python scoring (all comparison lives here)

**Files:** new `tests/http/test_visual_regression.py`, `tests/http/visual.py`; `tests/http/requirements.txt` (+`pillow`, `scikit-image`, `numpy`); goldens at `tests/http/goldens/<recording>/<variant>/<checkpointId>.png`; thresholds at `tests/http/goldens/<recording>/thresholds.json`.

Follows the existing skip-safe conventions in `tests/http/conftest.py` (session skip when no server reachable; `require_tool` / `require_enum` gating against the live schema).

**Skip conditions:** `capture` absent from the live tool schema; or `inspect kind=registrants → capabilities.capture` empty while the recording forbids native.

**Flow:**

1. `record {action:"replay", path, restoreScene:true, variant:"<variant>"}` → `{runId}`.
2. Poll `record {action:"status", runId}` until `done`.
3. Walk `result.results[]` for `kind=="tool" && tool=="capture"`; each carries the full output object of 1d at `.result`.
4. For each, load `.result.path` (or read the sidecar directly when scanning the tree offline).
5. SSIM via `skimage.metrics.structural_similarity` against the golden; ROI crops/masks applied **in Python** (multiple independent regions, independent thresholds, no plugin change); per-checkpoint thresholds from `thresholds.json`.

**Inconclusive — never a failure — when any of:** `sceneMismatch:true`; non-empty `degraded[]`; `provider=="native"`; `uiExcluded` differs from the golden's recorded flag; a matching `capture.abandoned` event on `GET /api/events`.

**Golden refresh** is a separate opt-in mode (`--visual-update`) that copies capture paths over the goldens. Goldens are versioned in git alongside the recording.

---

## Sequencing

1. **Part 0** — registrant ledger + `inspect kind=registrants`. Self-contained; ships alone; closes a ROADMAP item.
2. **Part 1h** — `inspect kind=screenshots`. Shares the snapshot helper the native path needs, is independently useful, and validates assumption **A1** against a real install _before_ anything depends on it.
3. **Part 1a–1i** — `capture` tool, base-tool extension wiring, kind-resolution rule, native fallback, config. Testable standalone: `capture {kind:"native", checkpointId:"smoke"}` with no recording and no provider.
4. **Part 1j** — run-scoped injection in `ScenarioHandler` (non-const `args`, per-rep `runSceneMismatch`, `runId`/`repeat`/`sceneMismatch`). Trivial once `capture` exists; required before checkpoints mean anything.
5. **Part 2** — manifest `checkpoints` + `capabilities`, gate, macro expansion, `variant` arg, the `cumMs` accumulation test. Depends on 3 (gate reads `ToolExtensions::Keys("capture")`; macro emits `tool:"capture"`) and 4.
6. **Part 3** — Open Shaders provider. Depends only on the 1f contract being frozen and the build number bumped; can proceed in parallel with 5.
7. **Part 4** — pytest scoring. Needs 5 for end-to-end, but `visual.py` (SSIM, ROI, thresholds) can be written and unit-tested against static PNGs from day one.

**Version / ABI.** Bump `xmake.lua:18` to **1.14.0** (build 11400). **No new vtable slot** is added to `IDevBenchInterface001` — `capture` reuses the existing `RegisterToolExtension` slot — so the ABI is unchanged and only the _documented_ base-tool set grows. Update the doc comment at `include/DevBenchAPI.h:76-88`: "Opted-in base tools: `menu`, `inspect`, `capture`", plus the full provider contract from 1f.

---

## Deliberate non-goals

- No SSIM, pixel diffing, thresholds, pass/fail, or golden versioning in C++.
- No D3D11, staging textures, or image codecs in devbench.
- No new `scenario` step kind — checkpoints are a macro over existing primitives (`ROADMAP.md:311-318`).
- No capture-side ROI cropping by default; `subrect` is provider-only and opt-in.
- No HUD state manipulation from the step list; UI exclusion belongs to whoever owns the render surface.
- No per-tool→mod attribution in the registrant list (blocked on the per-plugin-interface ROADMAP item).
- No changes to the existing `openshaders.capture` top-level tool's behaviour.

---

## Review process note

This plan was produced via the standing three-stage process: a supervisor design pass (Opus) reading the actual current source, an adversarial review by a different model family (Gemini, via agy-bridge), and a rebuttal/reconciliation pass by the same supervisor agent addressing every finding individually. The orchestrator independently re-verified the highest-stakes disagreement (checkpoint anchoring via `atMs` vs. the reviewer's claim of wall-clock drift) against `Recording.cpp:236-268` and `685-701` before accepting the supervisor's rejection of that finding. Accepted fixes: request-queue re-arm and shutdown-safe callback clearing (Q1); exclusive-open readiness probe, copy retry, and a granularity-independent candidate rule (Q2); per-repetition scene-mismatch reset and per-repetition filename suffixing (Q4); capability-gate/macro consistency (Q5); a stricter "auto never silently resolves to native" rule (Q6); removal of the `tm` HUD toggle in favor of a provider-side `excludeUi` contract field (Q7); and a single capture-request queue as the sole entry point for hotkey, UI, and provider-triggered captures (Q8). Rejected: step-index checkpoint anchoring (Q3) — the `atMs` mechanism is resolved statically at plan time, not against a replay-time wall clock, so it does not have the drift failure mode the reviewer described.

### Critical files for implementation

- `E:\Documents\source\repos\devbench\src\Tools.cpp` — scenario dispatch 1500-1811; repeat loop 1534; tool branch 1669-1686; assert-scene 1738-1767; `InspectHandler` 624-1117; descriptor rebuild 2001-2016; `record` schema 2164-2177
- `E:\Documents\source\repos\devbench\src\Recording.cpp` — `BuildReplaySteps` 491-714; compat gate 581-592; trajectory copy + settle injection 690-701; `BuildScenario` wait derivation 237-268
- `E:\Documents\source\repos\devbench\src\HostApi.cpp` — interface impl 40-106; `OnInterfaceRequest` 157-163
- `E:\Documents\source\repos\open-shaders\src\Features\ScreenshotFeature.cpp` — ImGui trigger 703; `ProcessCaptureRequest` 826; `StopWorkerThread` 843; worker 865-925; `Capture` 935-1017
- `E:\Documents\source\repos\open-shaders\src\Features\RemoteControl\DevBenchBridge.cpp` — capture handler 721-787; `Install` 1013-1084
