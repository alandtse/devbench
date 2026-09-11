"""Opt-in VR free-camera regression test against a running, repaired DevBench.

Set DEVBENCH_TEST_FREECAM=1 and DEVBENCH_URL to the intended test instance.
The default skip happens before server discovery. An explicit URL and an already
loaded scene are required. The process identity and backend marker are rechecked
before every mutation, including cleanup after a failed assertion.

Run in a stationary, unobstructed scene without movement input. This checks the
camera-node transform after subsequent game frames; visually inspect both eyes
separately to qualify stereo presentation (docs/vr-free-camera.md).
"""

from __future__ import annotations

import math
import os
import time

import pytest
import requests

from conftest import require_enum, require_tool


VR_FREE_CAMERA_STATE_ID = 3


pytestmark = pytest.mark.skipif(
    os.environ.get("DEVBENCH_TEST_FREECAM") != "1" or not os.environ.get("DEVBENCH_URL"),
    reason="set DEVBENCH_TEST_FREECAM=1 and DEVBENCH_URL to the intended VR instance",
)


class _CameraSession:
    """Stop mutations permanently after an identity/backend/liveness failure.

    HTTP health and camera reads cannot make the following write atomic, but they
    prevent a detected restart from sending cleanup into a replacement process.
    """

    def __init__(self, client, health):
        self.client = client
        self.pid = health["pid"]
        self.frame = health["frame"]
        self.usable = True

    def health(self):
        assert self.usable, "camera session is no longer safe to mutate"
        try:
            health = self.client.ok("inspect", {"kind": "health"})
            assert health.get("vr") is True, health
            assert health["pid"] == self.pid, "game instance changed during camera test"
            assert health["frame"] >= self.frame, "game frame counter moved backwards"
            self.frame = health["frame"]
            return health
        except (AssertionError, AttributeError, KeyError, TypeError, requests.RequestException):
            self.usable = False
            raise

    def camera(self):
        self.health()
        try:
            camera = self.client.ok("camera", {"action": "get"})
            assert camera.get("freeCamBackend") == "vr-state", camera
        except (AssertionError, AttributeError, requests.RequestException):
            self.usable = False
            raise
        self.health()
        return camera

    def call(self, args):
        self.camera()
        return self.client.call("camera", args)

    def ok(self, args):
        status, result = self.call(args)
        assert status == 200, (status, result)
        return result

    def cleanup(self) -> None:
        if not self.usable:
            return
        status, result = self.call({"action": "freecam", "on": False})
        # Ownership or the scene may change after the last safety check.
        assert status in (200, 409), (status, result)


def _create_camera_session(client, tool_schema):
    """Validate the selected VR instance and scene before allowing mutations."""
    camera = require_tool(tool_schema, "camera")
    for action in ("get", "freecam", "drive"):
        require_enum(camera, "action", action)
    inspect = require_tool(tool_schema, "inspect")
    for kind in ("health", "scene"):
        require_enum(inspect, "kind", kind)

    health = client.ok("inspect", {"kind": "health"})
    initial = client.ok("camera", {"action": "get"})
    if initial.get("freeCamBackend") != "vr-state":
        pytest.skip("this instance does not advertise the repaired vr-state backend")

    session = _CameraSession(client, health)
    initial = session.camera()
    if initial.get("freeCam") is True:
        pytest.skip("free camera is already active; preserve its existing owner/view")
    assert initial.get("freeCam") is False, initial
    assert initial.get("freeCamOwned") is False, initial
    assert type(initial.get("stateId")) is int, initial
    assert initial["stateId"] != VR_FREE_CAMERA_STATE_ID, initial
    scene = client.ok("inspect", {"kind": "scene"})
    if scene.get("playerLoaded") is not True:
        pytest.skip("load a stationary scene before running the free-camera test")
    return initial, session


@pytest.fixture
def vr_camera_session(client, tool_schema):
    return _create_camera_session(client, tool_schema)


def _after_frames(session, count=3):
    start = session.health()
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        health = session.health()
        if health["frame"] >= start["frame"] + count:
            return session.camera()
        time.sleep(0.05)
    session.usable = False
    pytest.fail(f"game frames stopped advancing after {start['frame']}")


def _set_freecam(session, on):
    result = session.ok({"action": "freecam", "on": on})
    assert result.get("queued") is False, result
    assert result.get("action") == "freecam", result
    assert result.get("on") is on, result
    assert result.get("freeCam") is on, result


def _drive(session, position, pitch, yaw):
    result = session.ok({
        "action": "drive",
        "x": position[0], "y": position[1], "z": position[2],
        "pitch": pitch, "yaw": yaw,
    })
    assert result.get("queued") is False, result
    assert result.get("action") == "drive", result


def _assert_position(camera, expected):
    actual = [camera[key] for key in ("camX", "camY", "camZ")]
    assert actual == pytest.approx(expected, abs=0.25, rel=0), camera


def _angles(camera):
    angles = [camera[key] for key in ("camPitch", "camYaw")]
    assert all(math.isfinite(value) for value in angles), camera
    return angles


def _angle_distance(a, b):
    return abs(math.atan2(math.sin(a - b), math.cos(a - b)))


def test_vr_drive_requires_free_camera(vr_camera_session):
    initial, session = vr_camera_session
    status, result = session.call({
        "action": "drive", "x": 100.0, "y": 50.0, "z": 25.0,
        "pitch": 0.15, "yaw": 0.25,
    })
    assert status == 409, (status, result)
    current = session.camera()
    assert current["stateId"] == initial["stateId"], current
    assert current["freeCam"] is False, current


@pytest.mark.parametrize("field", ["x", "y", "z", "pitch", "yaw"])
def test_vr_drive_rejects_float_overflow(vr_camera_session, field):
    initial, session = vr_camera_session
    status, result = session.call({"action": "drive", field: 1e39})
    assert status == 400, (status, result)
    current = session.camera()
    assert current["stateId"] == initial["stateId"], current
    assert current["freeCam"] is False, current


def test_vr_free_camera_drive_persists_and_restores(client, vr_camera_session):
    initial, session = vr_camera_session
    scene = client.ok("inspect", {"kind": "scene"})
    assert scene.get("playerLoaded") is True, scene
    player_position = scene["position"]
    initial_position = [initial[key] for key in ("camX", "camY", "camZ")]

    try:
        for cycle in range(3):
            _set_freecam(session, True)
            _set_freecam(session, True)  # Must not replace the retained return state.
            active = _after_frames(session)
            assert active["freeCam"] is True and active["stateId"] == VR_FREE_CAMERA_STATE_ID, active
            assert active["freeCamOwned"] is True, active

            # Establish a known orientation before testing combined pitch/yaw;
            # world Euler readback need not equal native free-camera angles.
            _drive(session, initial_position, pitch=0.0, yaw=0.0)
            baseline = _after_frames(session)
            _assert_position(baseline, initial_position)
            baseline_angles = _angles(baseline)

            target = [
                initial_position[0] + 75.0 + cycle * 10.0,
                initial_position[1] + 25.0,
                initial_position[2] + 15.0,
            ]
            _drive(session, target, pitch=0.0, yaw=0.25)
            yaw_only = _after_frames(session)
            _assert_position(yaw_only, target)
            yaw_angles = _angles(yaw_only)
            assert _angle_distance(baseline_angles[1], yaw_angles[1]) > 0.05, (baseline, yaw_only)
            assert _angle_distance(baseline_angles[0], yaw_angles[0]) < 0.01, (baseline, yaw_only)

            _drive(session, target, pitch=0.15, yaw=0.25)
            driven = _after_frames(session)
            assert driven["freeCam"] is True and driven["stateId"] == VR_FREE_CAMERA_STATE_ID, driven
            assert driven["freeCamOwned"] is True, driven
            _assert_position(driven, target)
            driven_angles = _angles(driven)
            assert _angle_distance(yaw_angles[0], driven_angles[0]) > 0.05, (yaw_only, driven)

            held = _after_frames(session, count=12)
            _assert_position(held, target)
            for before, after in zip(driven_angles, _angles(held)):
                assert _angle_distance(before, after) < 0.01, (driven, held)
            current_scene = client.ok("inspect", {"kind": "scene"})
            assert current_scene["position"] == pytest.approx(
                player_position, abs=0.25, rel=0,
            ), current_scene

            _set_freecam(session, False)
            _set_freecam(session, False)
            restored = _after_frames(session)
            assert restored["freeCam"] is False, restored
            assert restored["freeCamOwned"] is False, restored
            assert restored["stateId"] == initial["stateId"], (initial, restored)
    finally:
        session.cleanup()
