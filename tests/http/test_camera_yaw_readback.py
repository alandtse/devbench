"""Opt-in: camera get reports the yaw/pitch that camera drive wrote.

Set DEVBENCH_TEST_FREECAM=1 and DEVBENCH_URL to the intended test instance. Flat
runtimes only (VR's free camera has its own opt-in test). Run in a loaded scene with
no movement input; the free camera is switched off again afterwards.
"""

from __future__ import annotations

import math
import os

import pytest

from conftest import require_tool


pytestmark = [
    pytest.mark.requires_player,
    pytest.mark.skipif(
        os.environ.get("DEVBENCH_TEST_FREECAM") != "1" or not os.environ.get("DEVBENCH_URL"),
        reason="set DEVBENCH_TEST_FREECAM=1 and DEVBENCH_URL to the intended instance",
    ),
]


def angle_diff(a: float, b: float) -> float:
    return abs((a - b + math.pi) % (2 * math.pi) - math.pi)


def test_get_reads_back_driven_angles(client, tool_schema):
    require_tool(tool_schema, "camera")
    if client.ok("inspect", {"kind": "state"}).get("vr"):
        pytest.skip("flat runtimes only")

    client.ok("camera", {"action": "freecam", "on": True})
    try:
        start = client.ok("camera", {"action": "get"})
        for yaw in (-3.0, -2.0, -1.0, -0.5, -0.1, 0.0, 0.1, 0.5, 1.0, 2.0, 3.0):
            client.ok(
                "camera",
                {"action": "drive", "x": start["camX"], "y": start["camY"], "z": start["camZ"], "pitch": 0.0, "yaw": yaw},
            )
            got = client.ok("camera", {"action": "get"})
            assert got["camAngles"] == "freeCameraState", got
            assert angle_diff(got["camYaw"], yaw) < 1e-3, (yaw, got)
            assert abs(got["camPitch"]) < 1e-3, (yaw, got)
    finally:
        client.ok("camera", {"action": "freecam", "on": False})
