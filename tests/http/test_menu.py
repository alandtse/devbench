"""Tests for the `menu` tool.

The 'open' action is build-dependent (absent in some branches); the open/close
round-trip test guards on the live action enum and skips when 'open' is missing.
"""

from __future__ import annotations

import time

import pytest

from conftest import require_enum, require_tool, schema_enum


@pytest.fixture
def menu(tool_schema):
    return require_tool(tool_schema, "menu")


def test_list(client, menu):
    require_enum(menu, "action", "list")
    body = client.ok("menu", {"action": "list"})
    assert isinstance(body, dict), body
    assert isinstance(body.get("openMenus"), list), body
    assert isinstance(body.get("messageBoxOpen"), bool), body


@pytest.mark.requires_player
def test_open_tween_roundtrip(client, menu):
    require_enum(menu, "action", "list")
    if "open" not in schema_enum(menu, "action"):
        pytest.skip("menu 'open' action not in live schema enum")

    target = "TweenMenu"

    def list_menus():
        return client.ok("menu", {"action": "list"}).get("openMenus", [])

    def wait_for(predicate, attempts=10, delay=0.3):
        for _ in range(attempts):
            menus = list_menus()
            if predicate(menus):
                return menus
            time.sleep(delay)
        return list_menus()

    # Open, then confirm it shows up (UI queue is async — poll).
    client.ok("menu", {"action": "open", "name": target})
    opened = wait_for(lambda m: target in m)
    assert target in opened, f"{target} not in openMenus after open: {opened}"

    # Close, then confirm it's gone.
    client.ok("menu", {"action": "close", "name": target})
    closed = wait_for(lambda m: target not in m)
    assert target not in closed, f"{target} still in openMenus after close: {closed}"
