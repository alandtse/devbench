"""Capability-safe checks for the versioned synthetic input interface.

These tests deliberately do not inject a key: discovery/status is safe at the main menu and
on unattended CI, while mutation belongs in an explicitly owned live automation scenario.
"""

from __future__ import annotations

from conftest import require_enum, require_tool


def test_input_capability_contract(client, tool_schema):
    desc = require_tool(tool_schema, "input")
    require_enum(desc, "action", "capabilities")
    body = client.ok("input", {"action": "capabilities"})
    assert body.get("contract") == {
        "name": "devbench.input",
        "version": {"major": 2, "minor": 0},
    }, body
    keyboard = body.get("capabilities", {}).get("keyboard", {})
    assert keyboard.get("available") is True, body
    assert keyboard.get("encoding") == "DirectInputScanCode", body
    assert keyboard.get("injection") == "Skyrim.BSInputEventQueue", body
    assert {"status", "down", "up", "tap", "sequence", "releaseAll"} <= set(
        keyboard.get("actions", [])
    ), body
    keys = keyboard.get("keys")
    assert isinstance(keys, list) and keys, body
    by_name = {k.get("key"): k for k in keys}
    enter = by_name.get("enter")
    assert enter is not None, body
    assert enter["scancode"] == 0x1C, enter
    assert enter["codeDomain"] == "directInputScanCode", enter
    assert enter["engineDevice"] == "keyboard", enter
    left = by_name.get("mouseLeft")
    assert left is not None, body
    assert left["scancode"] == 256, left
    assert left["codeDomain"] == "mouseButton", left
    assert left["engineDevice"] == "mouse", left
    assert {"mouseRight", "mouseMiddle"} <= set(by_name), body
    vr_set = body.get("capabilities", {}).get("vrTrackedSet", {})
    assert vr_set.get("atomicDevices") == ["hmd", "left", "right"], body
    assert vr_set.get("passThroughWhenInactive") is True, body
    assert "observe" in vr_set.get("actions", []), body


def test_keyboard_input_status_is_safe_without_player(client, tool_schema):
    desc = require_tool(tool_schema, "input")
    require_enum(desc, "action", "status")
    body = client.ok("input", {"action": "status", "device": "keyboard"})
    assert body.get("contract") == {
        "name": "devbench.input",
        "version": {"major": 2, "minor": 0},
    }, body
    assert isinstance(body.get("ready"), bool), body
    assert isinstance(body.get("held"), list), body
