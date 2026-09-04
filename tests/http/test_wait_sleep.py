"""Tests for the `wait` and `sleep` tools.

Completion is synchronous — the call itself drives the wait/sleep to done before
returning, no polling needed.
"""

from __future__ import annotations

import time

import pytest

from conftest import require_tool


@pytest.fixture
def wait(tool_schema):
    return require_tool(tool_schema, "wait")


@pytest.fixture
def sleep(tool_schema):
    return require_tool(tool_schema, "sleep")


# Vanilla, light interior with no hostiles/ownership gates -- forced fresh in each
# live test (not just relying on the suite's own bootstrap cell) so a prior test's
# console fiddling can't leave the player somewhere that spuriously refuses.
SAFE_COC_CELL = "WhiterunDragonsreach"


def _goto_safe_cell(client, timeout: float = 15.0) -> None:
    # console queues the coc command and returns before the cell transition
    # completes, so poll for the target cell rather than assuming it landed.
    client.ok("console", {"command": f"coc {SAFE_COC_CELL}"})
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        scene = client.ok("inspect", {"kind": "scene"})
        cell = scene.get("cell") or {}
        if cell.get("editorId") == SAFE_COC_CELL:
            return
        time.sleep(0.5)
    pytest.fail(f"never reached cell '{SAFE_COC_CELL}' within {timeout}s")


@pytest.mark.parametrize("hours", [0, -1])
def test_wait_requires_positive_hours(client, wait, hours):
    status, body = client.call("wait", {"hours": hours})
    assert status == 400, (status, body)
    assert isinstance(body, dict) and "error" in body, body


def test_sleep_requires_positive_hours(client, sleep):
    status, body = client.call("sleep", {"hours": -1})
    assert status == 400, (status, body)
    assert isinstance(body, dict) and "error" in body, body


def test_wait_rejects_fractional_hours(client, wait):
    status, body = client.call("wait", {"hours": 1.5})
    assert status == 400, (status, body)
    assert isinstance(body, dict) and "error" in body, body


def test_wait_rejects_excessive_hours(client, wait):
    status, body = client.call("wait", {"hours": 1_000_000})
    assert status == 400, (status, body)
    assert isinstance(body, dict) and "error" in body, body


@pytest.mark.requires_player
def test_wait_advances_time(client, wait):
    _goto_safe_cell(client)
    before = client.ok("inspect", {"kind": "scene"}).get("daysPassed")
    assert isinstance(before, (int, float)), before
    body = client.ok("wait", {"hours": 1})
    assert isinstance(body, dict), body
    assert body.get("completed") is True, body
    assert body.get("hours") == 1, body
    after = client.ok("inspect", {"kind": "scene"}).get("daysPassed")
    assert isinstance(after, (int, float)) and after > before, (before, after)


@pytest.mark.requires_player
def test_sleep_advances_time(client, sleep):
    _goto_safe_cell(client)
    before = client.ok("inspect", {"kind": "scene"}).get("daysPassed")
    assert isinstance(before, (int, float)), before
    body = client.ok("sleep", {"hours": 1})
    assert isinstance(body, dict), body
    assert body.get("completed") is True, body
    after = client.ok("inspect", {"kind": "scene"}).get("daysPassed")
    assert isinstance(after, (int, float)) and after > before, (before, after)
