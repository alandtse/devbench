"""Shared fixtures for the devbench HTTP integration suite.

The suite is CI-safe: when no game/server is reachable the entire session is
SKIPPED (never failed). It also skips individual tests whose tool — or a needed
action/kind — is absent from the *live* schema, so the suite stays green across
branches that add or drop capabilities.

Self-contained: if a server is reachable but sitting at the main menu, the suite
drives the game into a playable state itself (see DEVBENCH_BOOTSTRAP below) so
requires_player tests run instead of skipping. An already-loaded game is used
as-is. Set DEVBENCH_BOOTSTRAP=off to keep the old "human controls state" behavior.

Discovery order for the base URL (first hit wins):
  1. env var DEVBENCH_URL  (e.g. http://127.0.0.1:8921)
  2. runtime.json under known Skyrim SE / VR Data paths ({"port": <int>})
  3. probe 127.0.0.1:8920..8925

Liveness is confirmed by POSTing {"kind":"state"} to /api/tool/inspect and
checking the response is JSON carrying a `plugin` field.
"""

from __future__ import annotations

import json
import os
import time
from pathlib import Path
from typing import Any, Iterable

import pytest
import requests

# Short timeouts keep the suite snappy when the server is up and fail fast when
# it is not. Game reads run on the main thread (which can stall mid-frame,
# especially in VR), so the liveness probe is forgiving and retried.
PROBE_TIMEOUT = 5.0
PROBE_RETRIES = 2
CALL_TIMEOUT = 15.0

PORT_RANGE = range(8920, 8926)  # 8920..8925 inclusive


# --------------------------------------------------------------------------- #
# Port / base-URL discovery
# --------------------------------------------------------------------------- #
def _runtime_json_candidates() -> Iterable[Path]:
    """Likely <GameData>/SKSE/Plugins/devbench/runtime.json locations.

    Covers the common Steam install roots for Skyrim SE and VR. Missing paths
    are simply skipped by the caller — this is best-effort discovery.
    """
    rel = Path("SKSE") / "Plugins" / "devbench" / "runtime.json"
    data_dirs = [
        r"C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\Data",
        r"C:\Program Files (x86)\Steam\steamapps\common\SkyrimVR\Data",
        r"E:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data",
        r"E:\SteamLibrary\steamapps\common\SkyrimVR\Data",
        r"D:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data",
        r"D:\SteamLibrary\steamapps\common\SkyrimVR\Data",
    ]
    for d in data_dirs:
        yield Path(d) / rel


def _read_runtime_port() -> int | None:
    for path in _runtime_json_candidates():
        try:
            if not path.is_file():
                continue
            data = json.loads(path.read_text(encoding="utf-8"))
            port = int(data["port"])
            if 1 <= port <= 65535:
                return port
        except (OSError, ValueError, KeyError, TypeError):
            continue
    return None


def _is_live(base_url: str) -> bool:
    """True if base_url's inspect{kind:state} returns JSON with a `plugin` field.

    Retried a few times: a connection *refused* fails fast (no server), but a
    live main thread stalled mid-frame can exceed a single probe timeout.
    """
    for _ in range(PROBE_RETRIES + 1):
        try:
            resp = requests.post(
                f"{base_url}/api/tool/inspect",
                json={"kind": "state"},
                timeout=PROBE_TIMEOUT,
            )
        except requests.ConnectionError:
            return False  # nothing listening — no point retrying this URL
        except requests.RequestException:
            continue  # timeout/other — main thread may be busy, retry
        if resp.status_code != 200:
            return False
        try:
            body = resp.json()
        except ValueError:
            return False
        return isinstance(body, dict) and "plugin" in body
    return False


def _discover_base_url() -> str | None:
    # 1. Explicit override.
    env = os.environ.get("DEVBENCH_URL")
    if env:
        env = env.rstrip("/")
        return env if _is_live(env) else None

    # 2. runtime.json published by a running instance.
    port = _read_runtime_port()
    if port is not None:
        url = f"http://127.0.0.1:{port}"
        if _is_live(url):
            return url

    # 3. Probe the default range (handles the port auto-increment).
    for p in PORT_RANGE:
        url = f"http://127.0.0.1:{p}"
        if _is_live(url):
            return url
    return None


# --------------------------------------------------------------------------- #
# Session-scoped fixtures
# --------------------------------------------------------------------------- #
@pytest.fixture(scope="session")
def base_url() -> str:
    """Discovered devbench base URL, or skip the whole suite if unreachable."""
    url = _discover_base_url()
    if url is None:
        pytest.skip(
            "no reachable devbench server "
            "(set DEVBENCH_URL, or start Skyrim with the plugin on 8920..8925)"
        )
    return url


class Client:
    """Thin requests wrapper that returns (status_code, parsed_json)."""

    def __init__(self, base_url: str):
        self.base_url = base_url
        self._session = requests.Session()

    def tools(self) -> list[dict[str, Any]]:
        resp = self._session.get(f"{self.base_url}/api/tools", timeout=CALL_TIMEOUT)
        assert resp.status_code == 200, f"GET /api/tools -> {resp.status_code}"
        return resp.json()

    def call(self, tool: str, args: dict[str, Any] | None = None,
             timeout: float = CALL_TIMEOUT) -> tuple[int, Any]:
        resp = self._session.post(
            f"{self.base_url}/api/tool/{tool}",
            json=args if args is not None else {},
            timeout=timeout,
        )
        try:
            body = resp.json()
        except ValueError:
            body = None
        return resp.status_code, body

    def ok(self, tool: str, args: dict[str, Any] | None = None,
           timeout: float = CALL_TIMEOUT) -> Any:
        """Call a tool, assert HTTP 200, and return the JSON result."""
        status, body = self.call(tool, args, timeout=timeout)
        assert status == 200, f"{tool}({args}) -> {status}: {body}"
        return body


@pytest.fixture(scope="session")
def client(base_url: str) -> Client:
    return Client(base_url)


@pytest.fixture(scope="session")
def tool_schema(client: Client) -> dict[str, dict[str, Any]]:
    """Live tool descriptors keyed by name: {name: {description, inputSchema, ...}}."""
    return {d["name"]: d for d in client.tools()}


# --------------------------------------------------------------------------- #
# Recipes — the portable scenario JSONs under recipes/ are the bench's reusable
# game-driving artifacts. One loader + one runner, shared by the recipe tests AND
# the env bootstrap below, so "how to play a recipe" lives in exactly one place.
# --------------------------------------------------------------------------- #
RECIPES_DIR = Path(__file__).parent / "recipes"


def load_recipe(name: str) -> dict[str, Any]:
    """Load recipes/<name>.json."""
    return json.loads((RECIPES_DIR / f"{name}.json").read_text(encoding="utf-8"))


def run_recipe(client: "Client", recipe: "str | dict[str, Any]", *,
               timeout: float = 120.0, continue_on_error: bool = True) -> tuple[int, Any]:
    """Play a recipe's steps through the `scenario` tool. `recipe` is a name or a
    loaded dict. Returns (status, body) — the single place that knows how to run one."""
    if isinstance(recipe, str):
        recipe = load_recipe(recipe)
    return client.call(
        "scenario",
        {"steps": recipe["steps"], "continueOnError": continue_on_error},
        timeout=timeout,
    )


# --------------------------------------------------------------------------- #
# Environment bootstrap — drive the game into a playable state itself, so
# requires_player tests run whether the game starts at the main menu or in-world.
#
#   DEVBENCH_BOOTSTRAP = coc (default) | save | off
#     coc  — `coc <cell>` from the main menu (save-independent; starts a fresh
#            session). Cell from DEVBENCH_COC_CELL (default "WhiterunDragonsreach").
#     save — load DEVBENCH_SAVE, or the most recent save if unset.
#     off  — never drive the game; tests skip if no save is loaded (old behavior).
#
# Bootstrapping only happens FROM THE MAIN MENU — an already-loaded game is used
# as-is and never disturbed. In CI (no server) the suite is skipped before this
# ever runs.
# --------------------------------------------------------------------------- #
BOOTSTRAP_TIMEOUT = 90.0
DEFAULT_COC_CELL = "WhiterunDragonsreach"  # vanilla, light, present in any load order


def _state_player_loaded(client: Client) -> bool:
    try:
        body = client.ok("inspect", {"kind": "state"})
    except (AssertionError, requests.RequestException):
        return False
    return bool(isinstance(body, dict) and body.get("playerLoaded"))


def _most_recent_save(client: Client) -> str | None:
    """Best-effort newest save name from `game list` (first entry), or None."""
    try:
        _, body = client.call("game", {"action": "list"})
        saves = body.get("saves") if isinstance(body, dict) else None
        return saves[0] if isinstance(saves, list) and saves else None
    except requests.RequestException:
        return None


def _clear_gating_modal(client: Client) -> None:
    """A load can be gated by a content-mismatch Yes/No box — accept it so the
    load proceeds. Best-effort: only acts when a MessageBoxMenu is actually open."""
    try:
        _, body = client.call("menu", {"action": "list"})
        if isinstance(body, dict) and body.get("messageBoxOpen"):
            client.call("menu", {"action": "accept", "index": 0})
    except requests.RequestException:
        pass


def _wait_player(client: Client, retrigger=None, timeout: float = BOOTSTRAP_TIMEOUT) -> bool:
    """Poll until a player is loaded; clear gating modals; retrigger once midway
    (a load/coc issued at the main menu sometimes doesn't take on the first try)."""
    deadline = time.time() + timeout
    retried = False
    while time.time() < deadline:
        _clear_gating_modal(client)
        if _state_player_loaded(client):
            return True
        if retrigger and not retried and (deadline - time.time()) < timeout - 25:
            retrigger()
            retried = True
        time.sleep(3)
    return False


def _bootstrap(client: Client) -> bool:
    mode = os.environ.get("DEVBENCH_BOOTSTRAP", "coc").lower()
    if mode == "off":
        return False
    if mode == "save":
        name = os.environ.get("DEVBENCH_SAVE") or _most_recent_save(client)
        if not name:
            return False
        load = lambda: client.call("game", {"action": "load", "name": name})  # noqa: E731
        load()
        return _wait_player(client, retrigger=load)
    # default "coc": reuse the portable `bootstrap` recipe (coc + waitUntil playerLoaded)
    # through the scenario engine — the same machinery the recipe tests use.
    cell = os.environ.get("DEVBENCH_COC_CELL", DEFAULT_COC_CELL)
    try:
        recipe = load_recipe("bootstrap")
        for step in recipe.get("steps", []):
            cmd = step.get("args", {}).get("command", "")
            if step.get("tool") == "console" and cmd.startswith("coc "):
                step["args"]["command"] = f"coc {cell}"
        status, _ = run_recipe(client, recipe, timeout=BOOTSTRAP_TIMEOUT)
        if status == 200 and _state_player_loaded(client):
            return True
    except (OSError, KeyError, requests.RequestException):
        pass  # no scenario tool / recipe missing → fall back to driving it directly
    coc = lambda: client.call("console", {"command": f"coc {cell}"})  # noqa: E731
    coc()
    return _wait_player(client, retrigger=coc)


@pytest.fixture(scope="session")
def player_loaded(client: Client) -> bool:
    """True once an in-world save is loaded. If the game is at the main menu, drive
    it into a playable state per DEVBENCH_BOOTSTRAP (default: coc) so the suite is
    self-contained; an already-loaded game is used as-is."""
    if _state_player_loaded(client):
        return True
    return _bootstrap(client)


# --------------------------------------------------------------------------- #
# Skip helpers (importable by test modules)
# --------------------------------------------------------------------------- #
def require_tool(tool_schema: dict[str, dict[str, Any]], name: str) -> dict[str, Any]:
    """Return the descriptor for `name`, or skip if it's not in the live schema."""
    if name not in tool_schema:
        pytest.skip(f"tool '{name}' not present in live /api/tools")
    return tool_schema[name]


def schema_enum(descriptor: dict[str, Any], prop: str) -> list[Any]:
    """Enum values declared for an input property, or [] if none/unknown."""
    try:
        return list(descriptor["inputSchema"]["properties"][prop]["enum"])
    except (KeyError, TypeError):
        return []


def require_enum(descriptor: dict[str, Any], prop: str, value: str) -> None:
    """Skip unless `value` is in the input property's enum.

    A missing enum is treated as "unconstrained" (no skip) — only an explicit
    enum that omits the value triggers a skip.
    """
    values = schema_enum(descriptor, prop)
    if values and value not in values:
        pytest.skip(f"action/kind '{value}' not in live schema enum for '{prop}' ({values})")


# --------------------------------------------------------------------------- #
# requires_player marker / fixture
# --------------------------------------------------------------------------- #
def pytest_configure(config: pytest.Config) -> None:
    config.addinivalue_line(
        "markers",
        "requires_player: test needs an in-world save; the suite bootstraps one "
        "(DEVBENCH_BOOTSTRAP) and only skips if that's disabled or fails",
    )


@pytest.fixture
def requires_player(player_loaded: bool) -> None:
    """Skip only if no player is loaded and the bootstrap couldn't establish one
    (DEVBENCH_BOOTSTRAP=off, or it timed out)."""
    if not player_loaded:
        pytest.skip("no player loaded and bootstrap did not establish one "
                    "(DEVBENCH_BOOTSTRAP=off or it failed)")


@pytest.fixture(autouse=True)
def _auto_requires_player(request: pytest.FixtureRequest) -> None:
    """Honor the @pytest.mark.requires_player marker without an explicit fixture."""
    if request.node.get_closest_marker("requires_player"):
        request.getfixturevalue("requires_player")
