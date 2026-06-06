"""Scenario-tool test: replay a portable navigation recipe end-to-end.

`recipes/dragonsreach_navigate.json` is a ~31s recipe derived from a devbench
recording but made save-agnostic: it `coc`s into Whiterun Dragonsreach, waits
for the load, then replays the recorded player.setpos/setangle navigation. So it
runs against *any* loaded save, not just the one it was recorded on.

This is the heaviest e2e test (it really drives the game for ~31s), so it is
marked `slow` — deselect with `-m "not slow"`.
"""

from __future__ import annotations

import pytest

from conftest import load_recipe, require_tool, run_recipe


@pytest.mark.slow
@pytest.mark.requires_player
def test_navigate_dragonsreach_recipe(client, tool_schema):
    require_tool(tool_schema, "scenario")
    require_tool(tool_schema, "console")  # the recipe dispatches console steps

    recipe = load_recipe("dragonsreach_navigate")
    steps = recipe["steps"]
    assert len(steps) > 50, "recipe should be the full navigation, not a stub"

    # The scenario blocks server-side for the recipe's duration (~31s) — give the
    # HTTP call room beyond that.
    status, body = run_recipe(client, recipe, timeout=120.0)
    assert status == 200, body
    assert body.get("ok") is True, body
    assert body.get("aborted") in (False, None), body
    assert body.get("stepsRun") == len(steps), body
    # It actually drove the game for roughly the recorded duration.
    assert body.get("elapsedMs", 0) >= 10_000, body
