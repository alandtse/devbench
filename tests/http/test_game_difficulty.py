"""Tests for the `game` tool's getDifficulty and setDifficulty actions."""

from __future__ import annotations

import pytest

LEVELS = ["Novice", "Apprentice", "Adept", "Expert", "Master", "Legendary"]


@pytest.mark.parametrize("level", [-1, 6, 2.5, True, "survival", ""])
def test_set_difficulty_rejects_an_invalid_level(client, level):
    status, body = client.call("game", {"action": "setDifficulty", "level": level})
    assert status == 400, (status, body)
    assert isinstance(body, dict) and "error" in body, body


def test_set_difficulty_requires_a_level(client):
    status, body = client.call("game", {"action": "setDifficulty"})
    assert status == 400, (status, body)
    assert isinstance(body, dict) and "error" in body, body


@pytest.mark.requires_player
def test_set_difficulty_by_index_and_name(client):
    original = client.ok("game", {"action": "getDifficulty"})["level"]
    try:
        for index, name in enumerate(LEVELS):
            by_index = client.ok("game", {"action": "setDifficulty", "level": index})
            assert by_index == {"level": index, "name": name}, by_index
            assert client.ok("game", {"action": "getDifficulty"}) == by_index

        by_name = client.ok("game", {"action": "setDifficulty", "level": "expert"})
        assert by_name == {"level": 3, "name": "Expert"}, by_name
    finally:
        client.ok("game", {"action": "setDifficulty", "level": original})
