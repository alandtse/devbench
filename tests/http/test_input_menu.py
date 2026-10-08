"""Opt-in: injected keys reach menus that read keys in ActionScript (the Main Menu).

Set DEVBENCH_TEST_INPUT=1 and DEVBENCH_URL. Start the game to the Main Menu with at least one save and leave it there.
The test picks NEW with the arrow keys and confirms twice with Enter, so it STARTS A NEW GAME (nothing is saved).
Keys queued with a null user event never reached the Main Menu: MenuControls passed them on as an empty user event
instead of a Scaleform key event.
"""

from __future__ import annotations

import os
import time
import uuid

import pytest
import requests

from conftest import require_enum, require_tool


pytestmark = pytest.mark.skipif(
    os.environ.get("DEVBENCH_TEST_INPUT") != "1" or not os.environ.get("DEVBENCH_URL"),
    reason="set DEVBENCH_TEST_INPUT=1 and DEVBENCH_URL to the intended test instance",
)


def test_main_menu_takes_injected_arrows_and_enter(client, tool_schema):
    require_enum(require_tool(tool_schema, "input"), "action", "tap")
    require_tool(tool_schema, "scenario")
    if not client.ok("input", {"action": "status"})["ready"]:
        pytest.skip("keyboard input is not ready")
    if "Main Menu" not in client.ok("menu", {"action": "list"})["openMenus"]:
        pytest.skip("start the game to the Main Menu before running this test")
    if not client.ok("game", {"action": "list", "limit": 1})["count"]:
        pytest.skip("needs a save, so that NEW is not the Main Menu's first entry")
    owner = f"pytest-menu-{uuid.uuid4().hex}"

    def tap(key):
        return {"tool": "input", "args": {"action": "tap", "device": "keyboard", "key": key, "owner": owner}}

    def events(since):
        resp = requests.get(f"{client.base_url}/api/events", params={"since": since}, timeout=15)
        assert resp.status_code == 200, resp.status_code
        return resp.json()

    # Up stops on CONTINUE, the first entry, wherever the highlight starts; one Down is NEW. Without the arrows Enter
    # would answer CONTINUE's prompt and load a save, so a newGame event proves the arrows and Enter both arrived.
    # The cursor is taken before the first key: newGame may fire while the last tap is still releasing.
    cursor = events(2**63)["headSeq"]
    steps = [tap("up"), {"wait": 300}] * 4 + [tap("down"), {"wait": 300}, tap("enter"), {"wait": 1500}, tap("enter")]
    result = client.ok("scenario", {"steps": steps}, timeout=30)
    assert result.get("ok"), result
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        for ev in events(cursor)["events"]:
            if ev["topic"] == "lifecycle" and ev["data"].get("event") == "newGame":
                return
            cursor = max(cursor, ev["seq"])
        time.sleep(0.25)
    pytest.fail("no newGame event within 60 s of the injected keys")
