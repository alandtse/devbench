# devbench HTTP integration tests

A starter pytest suite that drives the devbench REST API against a **running**
Skyrim with the plugin loaded. It is **CI-safe**: when no game/server is
reachable, the whole suite is _skipped_ (never failed), and individual tests are
skipped when the capability they exercise isn't present in the live build.

## Requirements

```sh
pip install -r requirements.txt
```

(`pytest`, `requests` — Python 3.9+.)

## Running

```sh
pytest tests/http -v
```

If you have multiple game instances or a non-default port, point the suite at
the right server explicitly:

```sh
# PowerShell
$env:DEVBENCH_URL = "http://127.0.0.1:8921"; pytest tests/http -v

# bash
DEVBENCH_URL=http://127.0.0.1:8921 pytest tests/http -v
```

## How discovery works

The base URL is resolved in this order (first hit wins):

1. **`DEVBENCH_URL`** environment variable (e.g. `http://127.0.0.1:8921`).
2. **`runtime.json`** published by a running instance at
   `<GameData>/SKSE/Plugins/devbench/runtime.json` (`{"port": <int>}`) — known
   Steam SE/VR `Data` paths are probed. The plugin writes this so fixed-URL
   clients can find an auto-incremented port.
3. **Port probe** of `127.0.0.1:8920..8925` (the default 8920 auto-increments
   when busy, e.g. multiple game instances).

A candidate is only accepted once `POST /api/tool/inspect {"kind":"state"}`
returns JSON carrying a `plugin` field (liveness check).

## Skip rules (why the suite stays green)

- **No server reachable** → entire session skipped.
- **No in-world save loaded** → tests marked `@pytest.mark.requires_player` are
  skipped (checked via `inspect{kind:state}.playerLoaded`). The suite never
  loads a save itself — the human/another agent controls game state.
- **Capability absent on this branch** → a test skips when its tool is missing
  from `GET /api/tools`, or when a needed action/kind is absent from the live
  `inputSchema` enum (e.g. `menu` `open`).

## Layout

| File              | Covers                                                                                 |
| ----------------- | -------------------------------------------------------------------------------------- |
| `conftest.py`     | discovery, `base_url`/`client`/`tool_schema` fixtures, `requires_player`, skip helpers |
| `test_smoke.py`   | discovery (`/api/tools`, `inspect` present) + console/camera/game smoke                |
| `test_inspect.py` | `inspect` state / vm / scene / refs                                                    |
| `test_papyrus.py` | `papyrus` list / describe / call (globals + members) + error                           |
| `test_menu.py`    | `menu` list, and the open→list→close round-trip (guarded)                              |
| `examples/`       | hand-run example recipes (not collected by pytest) — see below                         |

## Examples

`examples/combat_arena.py` is a hand-run recipe (not a CI test — it mutates game
state heavily and runs ~1 min) that drives, forces, and **resolves** a 3-way
combat scene through the REST API. It `tgm`s the player, spawns three factions on
the player, then each tick gathers the survivors onto the player
(`ObjectReference.MoveTo`) and forces the 3-way (`Actor.StartCombat`), reporting
live per-team counts until one team is left standing.

```sh
# with an in-world save already loaded:
python tests/http/examples/combat_arena.py
# -> ... WINNER: Stormcloaks (3/5 survived)
```

Its header documents the Skyrim-engine constraints it works around (high-process
zone, scatter/de-aggro, unreliable faction hostility, `player.`-only console
commands). It relies on devbench padding omitted optional args — `MoveTo` and
`StartCombat` both have optionals and otherwise dispatch-but-no-op. Use it as a
template for scripting your own combat/AI scenarios.

## Environment variables

| Variable       | Effect                                                               |
| -------------- | -------------------------------------------------------------------- |
| `DEVBENCH_URL` | Skip discovery and use this base URL (e.g. `http://127.0.0.1:8921`). |
