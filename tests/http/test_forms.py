"""Tests for the `forms` tool: find and headParts."""

from __future__ import annotations

import pytest


@pytest.mark.parametrize(
    "args",
    [
        {"action": "nope"},
        {"action": "find"},
        {"action": "find", "formType": "Spell"},
        {"action": "find", "formType": "Armor", "slot": 29},
        {"action": "find", "formType": "Armor", "slot": "hair"},
        {"action": "find", "formType": "Weapon", "slot": 31},
        {"action": "find", "formType": "Armor", "headPartType": "Hair"},
        {"action": "find", "formType": "HeadPart", "headPartType": "Wig"},
        {"action": "find", "formType": "Weapon", "model": "x"},
        {"action": "find", "formType": "Armor", "limit": 0},
        {"action": "find", "formType": "Armor", "limit": 501},
        {"action": "find", "formType": "Armor", "offset": -1},
        {"action": "find", "formType": "Armor", "name": 5},
        {"action": "headParts"},
        {"action": "describe"},
        {"action": "plugins", "limit": 0},
        {"action": "plugins", "filter": 5},
        {"action": "asset"},
    ],
)
def test_forms_rejects_a_malformed_request(client, args):
    status, body = client.call("forms", args)
    assert status == 400, (status, body)
    assert isinstance(body, dict) and "error" in body, body


@pytest.mark.requires_player
def test_find_head_parts_filters_and_pages(client):
    page = client.ok("forms", {"action": "find", "formType": "headpart", "headPartType": "hair", "limit": 2})
    assert page["returned"] == len(page["forms"]) <= 2, page
    assert page["total"] >= page["returned"], page
    for part in page["forms"]:
        assert part["headPartType"] == "Hair", part
        assert part["formId"].startswith("0x") and part["localId"].startswith("0x"), part
        assert part["plugin"], part

    if page["truncated"]:
        rest = client.ok("forms", {"action": "find", "formType": "HeadPart", "headPartType": "Hair", "limit": 2, "offset": 2})
        assert rest["offset"] == 2, rest
        assert all(p["formId"] != q["formId"] for p in rest["forms"] for q in page["forms"]), (page, rest)


@pytest.mark.requires_player
def test_find_armor_by_slot_and_plugin(client):
    body = client.ok("forms", {"action": "find", "formType": "Armor", "slot": 31, "plugin": "skyrim.esm", "limit": 5})
    assert body["total"] >= 1, body
    for armor in body["forms"]:
        assert 31 in armor["slots"], armor
        assert armor["plugin"].lower() == "skyrim.esm", armor
        assert isinstance(armor["addons"], list), armor


@pytest.mark.requires_player
def test_head_parts_of_an_actor_base(client):
    body = client.ok(
        "papyrus",
        {"action": "call", "script": "Game", "function": "GetPlayer"},
    )
    parts = client.ok("forms", {"action": "headParts", "formId": body["returned"]["formId"]})
    assert parts["headParts"], parts
    assert all("headPartType" in p for p in parts["headParts"]), parts


@pytest.mark.requires_player
def test_plugins_report_their_headers(client):
    body = client.ok("forms", {"action": "plugins", "filter": "skyrim.esm", "limit": 5})
    assert body["total"] >= 1, body
    skyrim = next(p for p in body["plugins"] if p["name"].lower() == "skyrim.esm")
    assert skyrim["author"], skyrim
    assert skyrim["master"] is True and isinstance(skyrim["masters"], list), skyrim
    assert isinstance(skyrim["index"], int), skyrim


@pytest.mark.requires_player
def test_describe_an_idle_reads_its_animation_and_plugin(client):
    idle = client.ok("forms", {"action": "describe", "formId": "IdleWave"})
    assert idle["formType"] == "IDLE", idle
    assert idle["pluginInfo"]["name"].lower() == "skyrim.esm", idle
    for key in ("animFile", "animEvent", "loopMin", "loopMax", "replayDelay"):
        assert key in idle, (key, idle)
    animation = idle["animation"]
    assert animation["source"] == "behavior" and animation["found"] is True, animation
    assert animation["clip"] == "IdleWave" and animation["match"] == "exact", animation
    assert 1.5 < animation["durationSeconds"] < 3.0, animation
    assert animation["className"].startswith("hka") and animation["transformTracks"] > 0, animation


@pytest.mark.requires_player
def test_describe_an_idle_whose_clip_has_a_prefix_or_none(client):
    dance = client.ok("forms", {"action": "describe", "formId": "IdleCiceroDance1"})["animation"]
    assert dance["match"] == "noIdlePrefix" and dance["clip"] == "CiceroDance1", dance
    assert 6.0 < dance["durationSeconds"] < 7.5, dance

    root = client.ok("forms", {"action": "describe", "formId": "NPCIdleRoot"})["animation"]
    assert root["found"] is False and "note" in root, root


@pytest.mark.requires_player
def test_asset_reads_a_game_file(client):
    skeleton = client.ok("forms", {"action": "asset", "path": "Actors/Character/Character Assets/skeleton.nif"})
    assert skeleton["found"] is True and skeleton["bytes"] > 1000, skeleton
    assert "durationSeconds" not in skeleton, skeleton

    clip = client.ok("forms", {"action": "asset", "path": "Actors/Character/Animations/IdleWave.hkx"})
    assert clip["found"] is True and clip["className"].startswith("hka"), clip

    missing = client.ok("forms", {"action": "asset", "path": "Actors/Character/Animations/NoSuchClip.hkx"})
    assert missing["found"] is False, missing

    status, body = client.call("forms", {"action": "asset", "path": "../Skyrim.ini"})
    assert status == 200 and body["found"] is False, (status, body)


@pytest.mark.requires_player
def test_describe_rejects_an_unknown_form(client):
    status, body = client.call("forms", {"action": "describe", "formId": "0xDEADBEEF"})
    assert status == 404, (status, body)
