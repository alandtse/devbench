"""Tests for the `map` tool: the player's custom map marker and Location world markers."""

from __future__ import annotations

import pytest

from conftest import require_tool

WHITERUN = "WhiterunLocation"


@pytest.fixture
def map_tool(client, tool_schema):
    require_tool(tool_schema, "map")


@pytest.fixture
def restore_waypoint(client, map_tool):
    """Leaves the player's waypoint as the test found it."""
    before = client.ok("map", {"action": "get"})
    yield before
    if before["set"]:
        client.call("map", {"action": "setWaypoint", "x": before["x"], "y": before["y"], "z": before["z"], "worldspace": before["worldspace"]["formId"]})
    else:
        client.call("map", {"action": "clearWaypoint"})


@pytest.mark.parametrize(
    "args, fragment",
    [
        ({}, "action is required"),
        ({"action": "teleport"}, "unknown action"),
        ({"action": "locate"}, "needs a location"),
        ({"action": "locate", "location": WHITERUN, "x": 1}, "only a location"),
        ({"action": "setWaypoint"}, "location, or both x and y"),
        ({"action": "setWaypoint", "location": WHITERUN, "x": 1, "y": 2}, "not both"),
        ({"action": "setWaypoint", "x": "far", "y": 2}, "finite number"),
    ],
)
def test_a_bad_request_says_what_to_pass(client, map_tool, args, fragment):
    status, body = client.call("map", args)
    assert status == 400 and fragment in str(body), (status, body)


@pytest.mark.requires_player
def test_set_waypoint_by_position_reads_back_and_clears(client, restore_waypoint):
    status, first = client.call("map", {"action": "setWaypoint", "x": 1000.0, "y": -2000.0})
    if status == 409:
        pytest.skip("the player is in an interior, which has no worldspace")
    assert status == 200, (status, first)
    waypoint = first["waypoint"]
    assert waypoint["set"] is True and waypoint["worldspace"], first
    assert abs(waypoint["x"] - 1000.0) < 1 and abs(waypoint["y"] + 2000.0) < 1, first

    second = client.ok("map", {"action": "setWaypoint", "x": 3000.0, "y": 4000.0, "z": 5.0})
    assert second["previous"]["x"] == pytest.approx(1000.0, abs=1), second
    assert client.ok("map", {"action": "get"})["x"] == pytest.approx(3000.0, abs=1)

    cleared = client.ok("map", {"action": "clearWaypoint"})
    assert cleared["cleared"] is True and cleared["previous"]["set"] is True, cleared
    assert client.ok("map", {"action": "get"})["set"] is False
    assert client.ok("map", {"action": "clearWaypoint"})["cleared"] is False


@pytest.mark.requires_player
def test_a_location_resolves_to_its_world_marker(client, restore_waypoint):
    status, located = client.call("map", {"action": "locate", "location": WHITERUN})
    if status == 404 and "no world marker loaded" in str(located):
        pytest.skip("the player is not in the worldspace that holds the Whiterun marker")
    assert status == 200, (status, located)
    assert located["worldspace"] and located["location"]["formId"], located

    placed = client.ok("map", {"action": "setWaypoint", "location": WHITERUN})["waypoint"]
    assert placed["x"] == pytest.approx(located["x"], abs=1) and placed["y"] == pytest.approx(located["y"], abs=1), (placed, located)
    assert placed["worldspace"]["formId"] == located["worldspace"]["formId"], (placed, located)


@pytest.mark.requires_player
def test_an_unknown_location_is_404(client, map_tool):
    status, body = client.call("map", {"action": "locate", "location": "DevbenchNoSuchLocation"})
    assert status == 404 and "not found" in str(body), (status, body)


@pytest.mark.requires_player
def test_a_form_that_is_not_a_location_is_400(client, map_tool):
    status, body = client.call("map", {"action": "locate", "location": "0x14"})
    assert status == 400 and "not a Location" in str(body), (status, body)


@pytest.mark.requires_player
def test_an_unknown_worldspace_is_404(client, restore_waypoint):
    status, body = client.call("map", {"action": "setWaypoint", "x": 0, "y": 0, "worldspace": "DevbenchNoSuchWorld"})
    assert status == 404 and "worldspace not found" in str(body), (status, body)
