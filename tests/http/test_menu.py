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


def test_list_includes_registered(client, menu):
    # Consumer-registered menu handlers (C-ABI RegisterMenuHandler) surface under `registered`.
    if "invoke" not in schema_enum(menu, "action"):
        pytest.skip("menu 'invoke' action not in live schema (no extension support on this build)")
    body = client.ok("menu", {"action": "list"})
    assert isinstance(body.get("registered"), list), body


def test_invoke_selftest_roundtrips(client, menu):
    # The host registers a `devbench.selftest` handler through the public C-ABI (the ping pattern),
    # so invoke must route to it and echo the args back through the C-callback boundary.
    require_enum(menu, "action", "invoke")
    registered = client.ok("menu", {"action": "list"}).get("registered", [])
    if "devbench.selftest" not in registered:
        pytest.skip("devbench.selftest handler not registered on this build")
    body = client.ok("menu", {"action": "invoke", "name": "devbench.selftest", "foo": 42})
    assert body.get("invoked") is True, body
    assert body.get("echo", {}).get("foo") == 42, body

    desc = client.ok("menu", {"action": "describe", "name": "devbench.selftest"})
    assert desc.get("registered") is True and isinstance(desc.get("descriptor"), dict), desc


def test_invoke_unknown_menu_404(client, menu):
    require_enum(menu, "action", "invoke")
    status, body = client.call("menu", {"action": "invoke", "name": "NoSuchMenuXYZ"})
    assert status == 404, (status, body)
    assert isinstance(body, dict) and "error" in body, body


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
