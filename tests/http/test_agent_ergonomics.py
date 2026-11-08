"""Calls that agents get wrong in practice must either work or fail with a pointer to the fix."""

from __future__ import annotations

import time

import pytest

from conftest import require_tool


def test_wait_ms_pauses_in_real_time(client, tool_schema):
    require_tool(tool_schema, "wait")
    start = time.monotonic()
    body = client.ok("wait", {"ms": 300})
    assert body["completed"] is True, body
    assert body["waitedMs"] >= 290, body
    assert time.monotonic() - start >= 0.29


def test_wait_coerces_a_stringified_ms(client, tool_schema):
    require_tool(tool_schema, "wait")
    body = client.ok("wait", {"ms": "50"})
    assert body["completed"] is True, body


@pytest.mark.requires_player
def test_wait_until_player_loaded_returns_at_once(client, tool_schema):
    require_tool(tool_schema, "wait")
    body = client.ok("wait", {"until": "playerLoaded", "timeoutMs": 5000})
    assert body["completed"] is True and body["until"] == "playerLoaded", body


@pytest.mark.parametrize(
    "args, fragment",
    [
        ({}, "'hours'"),
        ({"until": "nonsense"}, "unknown waitUntil condition"),
        ({"ms": -1}, "'ms'"),
        ({"ms": 10, "until": "noMenu"}, "not a combination"),
        ({"until": "noMenu", "timeoutMs": 700000}, "'timeoutMs'"),
    ],
)
def test_wait_rejects_a_bad_request_with_a_hint(client, tool_schema, args, fragment):
    require_tool(tool_schema, "wait")
    status, body = client.call("wait", args)
    assert status == 400, (status, body)
    assert fragment in str(body), body


def test_sleep_hours_error_says_game_time(client, tool_schema):
    require_tool(tool_schema, "sleep")
    status, body = client.call("sleep", {"ms": 1000})
    assert status == 400 and "GAME-time" in str(body), (status, body)


def test_scenario_status_is_compact_and_pageable(client, tool_schema):
    require_tool(tool_schema, "scenario")
    queued = client.ok("scenario", {"steps": [{"wait": 10}] * 5, "async": True})
    run_id = queued["runId"]
    deadline = time.monotonic() + 15
    status = client.ok("scenario", {"action": "status", "runId": run_id})
    while not status.get("done") and time.monotonic() < deadline:
        time.sleep(0.2)
        status = client.ok("scenario", {"action": "status", "runId": run_id})
    assert status["done"] is True, status
    assert "results" not in status["result"], status
    assert status["result"]["resultsTotal"] == 5, status

    page = client.ok("scenario", {"action": "status", "runId": run_id, "from": 1, "limit": 2})
    assert len(page["result"]["results"]) == 2 and page["result"]["resultsFrom"] == 1, page

    full = client.ok("scenario", {"action": "status", "runId": run_id, "verbose": True})
    assert len(full["result"]["results"]) == 5, full

    listed = client.ok("scenario", {"action": "list"})
    assert run_id in [run["runId"] for run in listed["runs"]], listed


def test_capture_without_a_checkpoint_id_names_the_file(client, tool_schema):
    require_tool(tool_schema, "capture")
    status, body = client.call("capture", {"kind": "native", "timeoutMs": 1000})
    assert status in (200, 504), (status, body)
    if status == 200:
        assert body["checkpointId"].startswith("shot_"), body
    else:
        assert "no new screenshot" in body["error"] and "checkpointId 'shot_" in body["error"], body


def test_capture_accepts_screenshot_as_an_action(client, tool_schema):
    require_tool(tool_schema, "capture")
    args = {"kind": "native", "timeoutMs": 1000}
    plain_status, plain = client.call("capture", {**args, "action": "capture"})
    status, body = client.call("capture", {**args, "action": "screenshot"})
    assert status == plain_status, (status, body, plain_status, plain)
    assert "action" in tool_schema["capture"]["inputSchema"]["properties"]
    assert "screenshot" in tool_schema["capture"]["inputSchema"]["properties"]["action"]["enum"]


def test_burst_without_a_checkpoint_id_is_not_a_malformed_request(client, tool_schema):
    require_tool(tool_schema, "capture")
    status, body = client.call("capture", {"action": "burst", "count": 5})
    assert "checkpointId" not in str(body.get("error", "")), (status, body)


def test_forms_find_accepts_type_for_formtype(client, tool_schema):
    require_tool(tool_schema, "forms")
    page = client.ok("forms", {"action": "find", "type": "Armor", "limit": 1})
    assert page["returned"] == len(page["forms"]) <= 1, page


def test_game_load_accepts_save_for_name(client, tool_schema):
    require_tool(tool_schema, "game")
    status, body = client.call("game", {"action": "load", "save": "devbench-no-such-save"})
    assert status == 404 and "not found" in str(body), (status, body)


def test_unknown_tool_says_what_to_check(client):
    status, body = client.call("devbench_no_such_tool", {})
    assert status == 404 and "inspect kind=registrants" in str(body), (status, body)
