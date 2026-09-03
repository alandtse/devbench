"""Tests for the `wait` and `sleep` tools.

Completion is synchronous — the call itself drives the wait/sleep to done before
returning, no polling needed.
"""

from __future__ import annotations

import pytest

from conftest import require_tool


@pytest.fixture
def wait(tool_schema):
    return require_tool(tool_schema, "wait")


@pytest.fixture
def sleep(tool_schema):
    return require_tool(tool_schema, "sleep")


def test_wait_requires_positive_hours(client, wait):
    status, body = client.call("wait", {"hours": 0})
    assert status == 400, (status, body)
    assert isinstance(body, dict) and "error" in body, body


def test_sleep_requires_positive_hours(client, sleep):
    status, body = client.call("sleep", {"hours": -1})
    assert status == 400, (status, body)
    assert isinstance(body, dict) and "error" in body, body


@pytest.mark.requires_player
def test_wait_advances_time(client, wait):
    before = client.ok("inspect", {"kind": "scene"}).get("daysPassed")
    assert isinstance(before, (int, float)), before
    body = client.ok("wait", {"hours": 1})
    assert isinstance(body, dict), body
    if body.get("completed") is False:
        pytest.skip(f"wait refused: {body.get('reason')}")
    assert body.get("completed") is True, body
    assert body.get("hours") == 1, body
    after = client.ok("inspect", {"kind": "scene"}).get("daysPassed")
    assert isinstance(after, (int, float)) and after > before, (before, after)


@pytest.mark.requires_player
def test_sleep_advances_time(client, sleep):
    before = client.ok("inspect", {"kind": "scene"}).get("daysPassed")
    assert isinstance(before, (int, float)), before
    body = client.ok("sleep", {"hours": 1})
    assert isinstance(body, dict), body
    if body.get("completed") is False:
        pytest.skip(f"sleep refused: {body.get('reason')}")
    assert body.get("completed") is True, body
    after = client.ok("inspect", {"kind": "scene"}).get("daysPassed")
    assert isinstance(after, (int, float)) and after > before, (before, after)
