"""Tests for `input` device='vrTrackedSet' actions reach, reachStatus and reachStop."""

from __future__ import annotations

import time

import pytest

RIGHT_CALF = "NPC R Calf [RClf]"


def reach(client, **args):
    return client.call("input", {"device": "vrTrackedSet", "action": "reach", "owner": "reach-test", **args})


@pytest.mark.parametrize(
    "args",
    [
        {},
        {"hands": {}},
        {"hands": "right"},
        {"hands": {"right": {}}},
        {"hands": {"right": {"node": ""}}},
        {"hands": {"right": {"node": RIGHT_CALF, "front": 501}}},
        {"hands": {"right": {"node": RIGHT_CALF, "front": "4"}}},
        {"hands": {"right": {"node": RIGHT_CALF, "offset": [1, 2]}}},
        {"hands": {"right": {"node": RIGHT_CALF}}, "head": {"lookAt": ""}},
        {"hands": {"right": {"node": RIGHT_CALF}}, "head": {"back": 6}},
        {"hands": {"right": {"node": RIGHT_CALF}}, "holdMs": 50},
        {"hands": {"right": {"node": RIGHT_CALF}}, "iterations": 0},
        {"hands": {"right": {"node": RIGHT_CALF}}, "toleranceUnits": 0},
        {"head": {"lookAt": "hands"}},
    ],
)
def test_reach_rejects_a_malformed_request(client, args):
    status, body = reach(client, **args)
    assert status == 400, (status, body)
    assert isinstance(body, dict) and "error" in body, body


def test_reach_status_and_stop_without_a_run(client):
    status, body = client.call("input", {"device": "vrTrackedSet", "action": "reachStatus", "runId": 987654})
    assert status == 404, (status, body)
    status, body = client.call("input", {"device": "vrTrackedSet", "action": "reachStatus", "runId": "x"})
    assert status == 400, (status, body)
    body = client.ok("input", {"device": "vrTrackedSet", "action": "reachStop", "owner": "reach-test"})
    assert body.get("stopped") is False, body


def test_reach_is_refused_when_openvr_is_not_ready(client):
    state = client.ok("inspect", {"kind": "state"})
    if state.get("vr"):
        pytest.skip("VR runtime: OpenVR may be ready")
    status, body = reach(client, hands={"right": {"node": RIGHT_CALF}})
    assert status == 503, (status, body)


@pytest.mark.requires_player
def test_reach_places_a_hand_next_to_a_node(client):
    state = client.ok("inspect", {"kind": "state"})
    if not state.get("vr"):
        pytest.skip("needs a VR runtime")
    if not client.ok("input", {"device": "vrTrackedSet", "action": "status"}).get("ready"):
        pytest.skip("OpenVR tracked-set injection is not ready")

    status, started = reach(
        client,
        hands={"right": {"node": RIGHT_CALF, "front": 4}},
        head={"lookAt": "hands"},
        holdMs=2000,
        toleranceUnits=2,
    )
    assert status == 200 and started["queued"] is True, (status, started)
    run_id = started["runId"]

    status, second = reach(client, hands={"right": {"node": RIGHT_CALF}})
    assert status == 409, (status, second)

    deadline = time.monotonic() + 90
    body = {}
    while time.monotonic() < deadline:
        body = client.ok("input", {"device": "vrTrackedSet", "action": "reachStatus", "runId": run_id})
        if not body["active"]:
            break
        time.sleep(1)
    assert body["phase"] == "done", body
    assert body["right"]["errorDistance"] <= 2, body
    assert "error" not in body, body
