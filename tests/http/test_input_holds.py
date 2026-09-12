"""Opt-in held-key movement regression against a running game.

Set DEVBENCH_TEST_INPUT=1 and DEVBENCH_URL. Load an unpaused development save
with the player on foot and clear ground ahead, without other input. The tests
move the player forward and release their own keys without saving or reloading.
"""

from __future__ import annotations

import math
import os
import time
import uuid

import pytest

from conftest import require_enum, require_tool


pytestmark = pytest.mark.skipif(
    os.environ.get("DEVBENCH_TEST_INPUT") != "1" or not os.environ.get("DEVBENCH_URL"),
    reason="set DEVBENCH_TEST_INPUT=1 and DEVBENCH_URL to the intended test instance",
)


@pytest.mark.parametrize("release", ["up", "releaseAll", "expiry"])
def test_keyboard_hold_moves_until_release(client, tool_schema, release):
    desc = require_tool(tool_schema, "input")
    for action in ("status", "down", "up", "releaseAll"):
        require_enum(desc, "action", action)
    status = client.ok("input", {"action": "status"})
    if not status["ready"]:
        pytest.skip("keyboard input is not ready")
    if status["held"]:
        pytest.skip("release existing synthetic input before running movement tests")
    scene = client.ok("inspect", {"kind": "scene"})
    if not scene["playerLoaded"]:
        pytest.skip("load a development save before running movement tests")
    menus = client.ok("menu", {"action": "list"})
    if set(menus["openMenus"]) - {"HUD Menu", "Cursor Menu"}:
        pytest.skip("close menus before running movement tests")
    key = client.ok("papyrus", {
        "action": "call", "script": "Input", "function": "GetMappedKey",
        "args": ["Forward", 0],
    })["returned"]
    assert isinstance(key, int) and 0 < key < 256, key
    pid = client.ok("inspect", {"kind": "health"})["pid"]
    owner = f"pytest-keyboard-{uuid.uuid4().hex}"

    def send(action, **args):
        assert client.ok("inspect", {"kind": "health"})["pid"] == pid, "game instance changed"
        return client.ok("input", {"action": action, "owner": owner, **args})

    def position():
        current = client.ok("inspect", {"kind": "scene"})
        assert current["cell"] == scene["cell"], "player left the test cell"
        return current["position"][:2]

    try:
        start = position()
        send("down", key=key, maxHoldMs=1000 if release == "expiry" else 10000)
        time.sleep(0.25)
        first = position()
        time.sleep(0.25)
        second = position()
        assert math.dist(first, second) > 10, (first, second)
        assert math.dist(start, first) > 10, (start, first)
        if release == "up":
            result = send("up", key=key)
            assert result["released"], result
        elif release == "releaseAll":
            result = send("releaseAll")
            assert not result["failed"], result
            assert not result.get("pending"), result
            assert any(item["scancode"] == key for item in result["released"]), result
        else:
            time.sleep(0.7)
        time.sleep(0.25)
        stopped = position()
        time.sleep(0.3)
        after = position()
        assert math.dist(stopped, after) < 2, (stopped, after)
        assert not any(item["owner"] == owner for item in client.ok("input", {"action": "status"})["held"])
    finally:
        result = send("releaseAll")
        assert not result["failed"], result
