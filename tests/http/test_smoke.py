"""Discovery + light smoke tests for console / game / camera."""

from __future__ import annotations

import pytest

from conftest import require_enum, require_tool


# --------------------------------------------------------------------------- #
# Discovery
# --------------------------------------------------------------------------- #
def test_tools_listing_nonempty(client):
    tools = client.tools()
    assert isinstance(tools, list) and tools, "GET /api/tools returned an empty list"
    for d in tools:
        assert "name" in d, d
        assert "description" in d, d
        assert "inputSchema" in d, d
        assert "readOnly" in d, d


def test_inspect_present(tool_schema):
    assert "inspect" in tool_schema, sorted(tool_schema)


# --------------------------------------------------------------------------- #
# console
# --------------------------------------------------------------------------- #
def test_console_exec_queued(client, tool_schema):
    desc = require_tool(tool_schema, "console")
    require_enum(desc, "action", "exec")
    body = client.ok("console", {"action": "exec", "command": "getpos x"})
    assert body.get("queued") is True, body
    assert body.get("command") == "getpos x", body


# --------------------------------------------------------------------------- #
# camera
# --------------------------------------------------------------------------- #
def test_camera_get_pov(client, tool_schema):
    desc = require_tool(tool_schema, "camera")
    require_enum(desc, "action", "get")
    body = client.ok("camera", {"action": "get"})
    assert isinstance(body, dict), body
    assert "pov" in body, body
    # pov is a string (first|third|vanity|other) once a camera exists; may be
    # null at the main menu — accept either rather than flaking on game state.
    assert body["pov"] is None or isinstance(body["pov"], str), body


# --------------------------------------------------------------------------- #
# game
# --------------------------------------------------------------------------- #
def test_game_list_saves(client, tool_schema):
    desc = require_tool(tool_schema, "game")
    require_enum(desc, "action", "list")
    body = client.ok("game", {"action": "list"})
    assert isinstance(body, dict), body
    assert isinstance(body.get("saves"), list), body
    assert isinstance(body.get("count"), int), body
    assert body["count"] == len(body["saves"]), body
