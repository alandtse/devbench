"""Tests for `capture` action='burst' and action='status'."""

from __future__ import annotations

import pytest


@pytest.mark.parametrize(
    "args",
    [
        {"action": "nope", "checkpointId": "b"},
        {"action": "burst", "checkpointId": "b"},
        {"action": "burst", "checkpointId": "b", "count": 0},
        {"action": "burst", "checkpointId": "b", "count": 241},
        {"action": "burst", "checkpointId": "b", "count": 1.5},
        {"action": "burst", "checkpointId": "b", "count": 5, "everyNFrames": 0},
        {"action": "burst", "checkpointId": "b", "count": 5, "format": "gif"},
        {"action": "burst", "checkpointId": "b", "count": 5, "scale": 0.5},
        {"action": "burst", "checkpointId": "b", "count": 5, "excludeUi": "yes"},
        {"action": "burst", "checkpointId": "b", "count": 5, "subrect": {"x": 0.6, "y": 0, "w": 0.5, "h": 1}},
        {"action": "burst", "checkpointId": "b", "count": 5, "timeoutMs": 0},
        {"action": "burst", "count": 5},
        {"action": "burst", "checkpointId": "a/b", "count": 5},
        {"action": "status"},
        {"action": "status", "runId": 0},
        {"action": "status", "runId": "1"},
    ],
)
def test_burst_rejects_a_malformed_request(client, args):
    status, body = client.call("capture", args)
    assert status == 400, (status, body)
    assert isinstance(body, dict) and "error" in body, body


def test_status_of_an_unknown_run_is_404(client):
    status, body = client.call("capture", {"action": "status", "runId": 987654})
    assert status == 404, (status, body)


def test_burst_refuses_the_native_fallback(client):
    status, body = client.call("capture", {"action": "burst", "checkpointId": "b", "count": 5, "kind": "native"})
    assert status == 400, (status, body)
    assert "provider" in body["error"], body


def test_a_single_capture_still_needs_its_checkpoint_id(client):
    status, body = client.call("capture", {})
    assert status == 400, (status, body)
    assert "checkpointId" in body["error"], body
