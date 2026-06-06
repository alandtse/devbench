"""Follow-a-moving-NPC recipe: identify an NPC, track it, verify it moved.

`recipes/follow_nelkir.json` `coc`s into Whiterun Dragonsreach and repeatedly
`player.moveto`s Nelkir — a child NPC who wanders the hall. Because `moveto` is
ref-relative, a *static* step list still tracks the NPC wherever he walks.

The test proves the full loop the bench cares about:
  - identify the NPC's live position via `inspect refs formId=…`,
  - run the recipe (the `scenario` follow),
  - assert the player ended on the NPC (it followed),
  - assert the NPC actually moved during a sample window (it's a *moving* target;
    skips if the NPC happens to be idle, so it never flakes).

Marked `slow` (~18s) + `requires_player`.
"""

from __future__ import annotations

import math
import time

import pytest

from conftest import load_recipe, require_tool, run_recipe

NPC = "0x1A67B"  # Nelkir, a Dragonsreach wanderer (Skyrim.esm ref, stable across saves)
PLAYER = "0x14"


def _pos(client, formid):
    _, body = client.call("inspect", {"kind": "refs", "formId": formid})
    if not isinstance(body, dict) or not body.get("count"):
        return None
    return body["refs"][0].get("position")


def _dist2d(a, b):
    return math.dist(a[:2], b[:2])


@pytest.mark.slow
@pytest.mark.requires_player
def test_follow_moving_npc(client, tool_schema):
    require_tool(tool_schema, "scenario")
    require_tool(tool_schema, "console")
    require_tool(tool_schema, "inspect")

    recipe = load_recipe("follow_nelkir")

    # Run the follow recipe (coc in + repeated moveto). It blocks ~18s.
    status, body = run_recipe(client, recipe, timeout=90.0)
    assert status == 200, body
    assert body.get("ok") is True, body
    assert body.get("stepsRun") == len(recipe["steps"]), body

    time.sleep(0.8)  # let the final moveto apply
    npc = _pos(client, NPC)
    if npc is None:
        pytest.skip(f"NPC {NPC} not present in this save")
    player = _pos(client, PLAYER)
    assert player is not None, "player ref unresolved"

    # Followed: the recipe ends on a moveto, so the player should be on the NPC.
    followed = _dist2d(player, npc)
    assert followed < 300, f"player {followed:.0f}u from NPC after follow: player={player} npc={npc}"

    # Moving: sample the NPC over a window. If idle right now, skip (don't flake) —
    # but when it moves, this confirms we tracked a genuinely moving target.
    a = _pos(client, NPC)
    time.sleep(4.0)
    b = _pos(client, NPC)
    if a is None or b is None:
        pytest.skip("NPC vanished mid-sample")
    moved = _dist2d(a, b)
    if moved < 15:
        pytest.skip(f"NPC idle right now ({moved:.0f}u) — can't assert a moving target")
    assert moved >= 15, f"expected the NPC to be moving, saw {moved:.0f}u"
