"""Tests for the `papyrus` tool (list / describe / call into the live VM)."""

from __future__ import annotations

import numbers

import pytest

from conftest import require_enum, require_tool


def _is_number(v) -> bool:
    return isinstance(v, numbers.Number) and not isinstance(v, bool)


@pytest.fixture
def papyrus(tool_schema):
    return require_tool(tool_schema, "papyrus")


def test_list_filter_utility(client, papyrus):
    require_enum(papyrus, "action", "list")
    body = client.ok("papyrus", {"action": "list", "filter": "utility"})
    assert isinstance(body.get("total"), int) and body["total"] >= 1, body
    scripts = body.get("scripts")
    assert isinstance(scripts, list), body
    assert any("utility" in s.lower() for s in scripts), scripts


def test_describe_utility(client, papyrus):
    require_enum(papyrus, "action", "describe")
    body = client.ok("papyrus", {"action": "describe", "script": "Utility"})
    globals_ = body.get("globalFunctions")
    assert isinstance(globals_, list) and globals_, body
    match = [f for f in globals_ if f.get("name") == "GetCurrentGameTime"]
    assert match, "Utility.GetCurrentGameTime not found in globalFunctions"
    fn = match[0]
    assert fn.get("returnType") == "float", fn
    assert fn.get("params") == [], fn


@pytest.mark.requires_player
def test_call_utility_getcurrentgametime(client, papyrus):
    require_enum(papyrus, "action", "call")
    body = client.ok(
        "papyrus",
        {"action": "call", "script": "Utility", "function": "GetCurrentGameTime"},
    )
    assert body.get("called") is True, body
    assert _is_number(body.get("returned")), body
    assert body.get("returnedType") == "float", body


@pytest.mark.requires_player
def test_call_game_getplayer(client, papyrus):
    require_enum(papyrus, "action", "call")
    body = client.ok(
        "papyrus",
        {"action": "call", "script": "Game", "function": "GetPlayer"},
    )
    assert body.get("returned", {}).get("formId") == "0x00000014", body
    assert body.get("returnedType") == "Actor", body


def test_call_game_getgamesettingfloat(client, papyrus):
    require_enum(papyrus, "action", "call")
    body = client.ok(
        "papyrus",
        {
            "action": "call",
            "script": "Game",
            "function": "GetGameSettingFloat",
            "args": ["fJumpHeightMin"],
        },
    )
    assert _is_number(body.get("returned")), body


@pytest.mark.requires_player
def test_call_member_getactorvalue_health(client, papyrus):
    require_enum(papyrus, "action", "call")
    body = client.ok(
        "papyrus",
        {
            "action": "call",
            "script": "Actor",
            "function": "GetActorValue",
            "self": {"form": "0x14"},
            "args": ["Health"],
        },
    )
    assert _is_number(body.get("returned")), body


@pytest.mark.requires_player
def test_call_member_getnumitems(client, papyrus):
    require_enum(papyrus, "action", "call")
    body = client.ok(
        "papyrus",
        {
            "action": "call",
            "script": "ObjectReference",
            "function": "GetNumItems",
            "self": {"form": "0x14"},
        },
    )
    returned = body.get("returned")
    assert isinstance(returned, int) and not isinstance(returned, bool), body
    assert returned >= 0, body


def test_call_bogus_function_errors(client, papyrus):
    require_enum(papyrus, "action", "call")
    status, body = client.call(
        "papyrus",
        {"action": "call", "script": "Utility", "function": "NoSuchFunctionXYZ"},
    )
    # A non-existent function must fail cleanly with 404 (deterministic) and never
    # dispatch into the VM — an unresolved global dispatch is a CTD.
    assert status == 404, (status, body)
    assert isinstance(body, dict) and "error" in body, body
    assert body.get("code") == 404, body
