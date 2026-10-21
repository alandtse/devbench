"""Tests for the `game` tool's getAlwaysActive and setAlwaysActive actions."""

from __future__ import annotations

import pytest


@pytest.mark.parametrize("enabled", [None, 1, "yes", "true"])
def test_set_always_active_requires_a_boolean(client, enabled):
    args = {"action": "setAlwaysActive"}
    if enabled is not None:
        args["enabled"] = enabled
    status, body = client.call("game", args)
    assert status == 400, (status, body)
    assert isinstance(body, dict) and "error" in body, body


def test_set_always_active_round_trips_and_is_reported_by_state(client):
    original = client.ok("game", {"action": "getAlwaysActive"})["alwaysActive"]
    if original is None:
        pytest.skip("bAlwaysActive:General is not available in this runtime")
    try:
        for value in (True, False):
            result = client.ok("game", {"action": "setAlwaysActive", "enabled": value})
            assert result["alwaysActive"] is value, result
            assert client.ok("inspect", {"kind": "state"})["alwaysActive"] is value
    finally:
        client.ok("game", {"action": "setAlwaysActive", "enabled": original})
