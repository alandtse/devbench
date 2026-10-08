"""Tests for the `console` tool."""

from __future__ import annotations

import pytest

from conftest import require_tool


@pytest.fixture
def console(tool_schema):
    return require_tool(tool_schema, "console")


def test_exec_capture_returns_lines(client, console):
    body = client.ok("console", {"action": "exec", "command": "getgs fJumpHeightMin", "capture": True})
    assert body.get("completed") is True, body
    assert any("fJumpHeightMin" in line for line in body.get("lines", [])), body
