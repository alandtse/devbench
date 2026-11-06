"""Tests for the `gfx` tool. They read the HUD and write one scratch member on it, so a loaded game is enough."""

from __future__ import annotations

import pytest

from conftest import require_tool


HUD = "HUD Menu"


@pytest.fixture
def gfx(client, tool_schema):
    require_tool(tool_schema, "gfx")
    if HUD not in client.ok("menu", {"action": "list"})["openMenus"]:
        pytest.skip("needs the HUD open")


def test_get_reads_a_menu_with_a_budget(client, gfx):
    body = client.ok("gfx", {"op": "get", "menu": HUD, "path": "_root", "depth": 1})
    assert isinstance(body["value"], dict), body
    assert body["nodes"] > 0 and isinstance(body["truncated"], bool), body


def test_set_then_get_round_trips(client, gfx):
    path = "_root.devbenchGfxTest"
    client.ok("gfx", {"op": "set", "menu": HUD, "path": path, "value": 7})
    assert client.ok("gfx", {"op": "get", "menu": HUD, "path": path})["value"] == 7


def test_call_returns_the_value(client, gfx):
    body = client.ok("gfx", {"op": "call", "menu": HUD, "path": "_global.Math.max", "args": [2, 5]})
    assert body["value"] == 5, body


def test_batch_stops_at_the_first_failure(client, gfx):
    ops = [
        {"op": "get", "path": "_root"},
        {"op": "get", "path": "_root.devbenchGfxMissing.child"},
        {"op": "get", "path": "_root"},
    ]
    status, body = client.call("gfx", {"op": "batch", "menu": HUD, "ops": ops})
    assert status == 200, body
    assert body["ok"] is False and body["failedIndex"] == 1 and len(body["results"]) == 1, body


def test_a_closed_menu_is_404(client, gfx):
    status, _ = client.call("gfx", {"op": "get", "menu": "devbench no such menu", "path": "_root"})
    assert status == 404


def test_a_path_through_a_number_is_refused_not_a_crash(client, gfx):
    # GetMember on a number crashes the game in a release build, so this must stop at the type check.
    path = "_root.devbenchGfxNumber"
    client.ok("gfx", {"op": "set", "menu": HUD, "path": path, "value": 1})
    status, _ = client.call("gfx", {"op": "get", "menu": HUD, "path": path + ".child"})
    assert status == 400
