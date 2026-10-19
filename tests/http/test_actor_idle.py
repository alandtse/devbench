"""Tests for the `actor` tool: listIdles, playIdle, stopIdle."""

from __future__ import annotations

import time

import pytest

# A vanilla humanoid base with a leveled-actor template that takes idles; the same base the hair tests spawn.
NPC_BASE = "0xA2C8E"


@pytest.mark.parametrize(
    "args",
    [
        {"action": "nope", "formId": "0x14"},
        {"action": "playIdle"},
        {"action": "playIdle", "formId": "0x14"},
        {"action": "playIdle", "formId": "0x14", "idle": "IdleWave", "loop": True, "freezeAi": True},
        {"action": "playIdle", "formId": "0x14", "idle": "IdleWave", "repeatMs": 10},
        {"action": "playIdle", "formId": "0x14", "idle": "IdleWave", "repeatMs": 1.5},
        {"action": "listIdles", "formId": "0x14", "limit": 0},
    ],
)
def test_actor_rejects_a_malformed_request(client, args):
    status, body = client.call("actor", args)
    assert status == 400, (status, body)
    assert isinstance(body, dict) and "error" in body, body


@pytest.mark.requires_player
def test_actor_refuses_the_player(client):
    status, body = client.call("actor", {"action": "listIdles", "formId": "0x14"})
    assert status == 409, (status, body)


@pytest.mark.requires_player
def test_actor_reports_an_unknown_reference(client):
    status, body = client.call("actor", {"action": "listIdles", "formId": "0xDEADBEEF"})
    assert status == 404, (status, body)


def _spawn(client) -> str:
    """Place an NPC beside the player and return its reference as a hex FormID."""
    body = client.ok(
        "papyrus",
        {
            "action": "call",
            "script": "ObjectReference",
            "function": "PlaceAtMe",
            "self": {"form": "0x14"},
            "args": [{"form": NPC_BASE}, 1, False, False],
        },
    )
    return body["returned"]["formId"]


@pytest.mark.requires_player
def test_play_loop_and_stop_an_idle_on_a_spawned_npc(client):
    npc = _spawn(client)
    try:
        idles = client.ok("actor", {"action": "listIdles", "formId": npc, "limit": 5})
        assert idles["returned"] <= 5 and idles["total"] >= idles["returned"], idles

        played = client.ok("actor", {"action": "playIdle", "formId": npc, "idle": "IdleWave", "loop": True})
        assert played["ok"] and played["looping"], played
        assert client.ok("actor", {"action": "status", "formId": npc}) == {
            "ref": npc,
            "looping": True,
            "loopIdle": "IdleWave",
        }

        time.sleep(1)
        stopped = client.ok("actor", {"action": "stopIdle", "formId": npc})
        assert stopped["ok"], stopped
    finally:
        client.call(
            "papyrus",
            {"action": "call", "script": "ObjectReference", "function": "Disable", "self": {"form": npc}, "args": [False]},
        )
