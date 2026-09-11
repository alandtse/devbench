"""Offline safety regressions for the opt-in live camera test; no server needed."""

from __future__ import annotations

from pathlib import Path

import pytest
import requests

import test_camera_freecam as freecam


pytest_plugins = ["pytester"]


@pytest.mark.parametrize("enabled,url", [(None, "http://unused.invalid"), ("1", None)])
def test_missing_opt_in_skips_before_any_client_fixture(pytester, monkeypatch, enabled, url):
    for name, value in (("DEVBENCH_TEST_FREECAM", enabled), ("DEVBENCH_URL", url)):
        if value is None:
            monkeypatch.delenv(name, raising=False)
        else:
            monkeypatch.setenv(name, value)
    # Execute the real module in an isolated pytest session. Any premature
    # client/schema fixture or skip-helper resolution is a failure, never HTTP.
    pytester.makeconftest('''
import pytest
def require_tool(*args):
    pytest.fail("skip helper ran before opt-in")
def require_enum(*args):
    pytest.fail("skip helper ran before opt-in")
@pytest.fixture
def client():
    pytest.fail("client discovery ran before opt-in")
@pytest.fixture
def tool_schema():
    pytest.fail("schema discovery ran before opt-in")
''')
    pytester.makepyfile(test_opt_in=Path(freecam.__file__).read_text(encoding="utf-8"))
    result = pytester.runpytest("-q")
    result.assert_outcomes(skipped=7)


class FakeClient:
    def __init__(self):
        self.health = {"pid": 123, "frame": 100, "vr": True}
        self.camera = {
            "freeCamBackend": "vr-state", "freeCam": False,
            "freeCamOwned": False, "stateId": 0,
        }
        self.scene = {"playerLoaded": True}
        self.calls = []
        self.after_camera_read = None
        self.health_error = None
        self.camera_error = None

    def ok(self, tool, args):
        self.calls.append((tool, args.copy()))
        if tool == "inspect" and args["kind"] == "health":
            if self.health_error:
                raise self.health_error
            return self.health.copy()
        if tool == "inspect" and args["kind"] == "scene":
            return self.scene.copy()
        assert (tool, args) == ("camera", {"action": "get"})
        if self.after_camera_read:
            self.after_camera_read()
        return self.camera.copy()

    def call(self, tool, args):
        self.calls.append((tool, args.copy()))
        assert tool == "camera"
        if self.camera_error is not None:
            return self.camera_error
        if args["action"] == "freecam":
            self.camera.update(
                freeCam=args["on"], freeCamOwned=args["on"],
                stateId=freecam.VR_FREE_CAMERA_STATE_ID if args["on"] else 0,
            )
            return 200, {"queued": False, "action": "freecam", "on": args["on"], "freeCam": args["on"]}
        assert args["action"] == "drive"
        return 200, {"queued": False, "action": "drive"}

    @property
    def mutations(self):
        return [args for tool, args in self.calls if tool == "camera" and args["action"] != "get"]


@pytest.fixture
def fake_client():
    return FakeClient()


def _session(client):
    return freecam._CameraSession(client, client.health.copy())


def test_same_instance_enable_and_cleanup(fake_client):
    session = _session(fake_client)
    freecam._set_freecam(session, True)
    session.cleanup()
    assert fake_client.mutations == [
        {"action": "freecam", "on": True},
        {"action": "freecam", "on": False},
    ]
    assert fake_client.camera["freeCam"] is False


def test_cleanup_ownership_loss_preserves_original_failure(fake_client):
    session = _session(fake_client)
    freecam._set_freecam(session, True)
    fake_client.camera["freeCamOwned"] = False
    fake_client.camera_error = (409, {"error": "preserve the existing camera owner"})

    with pytest.raises(AssertionError, match="original camera assertion"):
        try:
            raise AssertionError("original camera assertion")
        finally:
            session.cleanup()

    assert fake_client.camera["freeCam"] is True
    assert fake_client.camera["freeCamOwned"] is False
    assert fake_client.mutations == [
        {"action": "freecam", "on": True},
        {"action": "freecam", "on": False},
    ]


def test_cleanup_does_not_accept_unexpected_error(fake_client):
    session = _session(fake_client)
    fake_client.camera_error = (500, {"error": "unexpected cleanup failure"})
    with pytest.raises(AssertionError, match="unexpected cleanup failure"):
        session.cleanup()


@pytest.mark.parametrize("change", ["pid", "frame", "vr", "backend", "unreachable"])
def test_changed_or_unreachable_instance_stops_mutations_and_cleanup(fake_client, change):
    session = _session(fake_client)
    if change == "pid":
        fake_client.health["pid"] += 1
    elif change == "frame":
        fake_client.health["frame"] -= 1
    elif change == "vr":
        fake_client.health["vr"] = False
    elif change == "backend":
        fake_client.camera["freeCamBackend"] = "engine"
    else:
        fake_client.health_error = requests.ConnectionError("test connection lost")

    with pytest.raises((AssertionError, requests.ConnectionError)):
        freecam._drive(session, [1, 2, 3], pitch=0.1, yaw=0.2)
    calls_after_failure = fake_client.calls.copy()
    session.cleanup()
    assert fake_client.calls == calls_after_failure
    assert fake_client.mutations == []


def test_restart_between_camera_read_and_mutation_is_detected(fake_client):
    session = _session(fake_client)
    fake_client.after_camera_read = lambda: fake_client.health.update(pid=456)
    with pytest.raises(AssertionError, match="instance changed"):
        freecam._set_freecam(session, True)
    session.cleanup()
    assert fake_client.mutations == []


def test_restart_first_detected_during_cleanup_does_not_toggle_replacement(fake_client):
    session = _session(fake_client)
    freecam._set_freecam(session, True)
    fake_client.health["pid"] = 456
    fake_client.camera["freeCamBackend"] = "engine"
    with pytest.raises(AssertionError, match="instance changed"):
        session.cleanup()
    assert fake_client.mutations == [{"action": "freecam", "on": True}]


def test_stalled_frames_stop_cleanup(fake_client, monkeypatch):
    session = _session(fake_client)
    ticks = iter([0.0, 9.0])
    monkeypatch.setattr(freecam.time, "monotonic", lambda: next(ticks))
    with pytest.raises(pytest.fail.Exception, match="frames stopped"):
        freecam._after_frames(session)
    session.cleanup()
    assert fake_client.mutations == []


@pytest.mark.parametrize("condition", ["old_backend", "already_active", "main_menu"])
def test_fixture_skips_unsuitable_scene_without_bootstrap_or_camera_mutation(fake_client, condition):
    if condition == "old_backend":
        fake_client.camera.pop("freeCamBackend")
    elif condition == "already_active":
        fake_client.camera.update(freeCam=True, stateId=freecam.VR_FREE_CAMERA_STATE_ID)
    else:
        fake_client.scene["playerLoaded"] = False

    schema = {name: {} for name in ("camera", "inspect")}
    with pytest.raises(pytest.skip.Exception):
        freecam._create_camera_session(fake_client, schema)
    assert fake_client.mutations == []
    assert all(tool in ("inspect", "camera") for tool, _ in fake_client.calls)


def test_fixture_detects_restart_during_initial_backend_check(fake_client):
    fake_client.after_camera_read = lambda: fake_client.health.update(pid=456)
    schema = {name: {} for name in ("camera", "inspect")}
    with pytest.raises(AssertionError, match="instance changed"):
        freecam._create_camera_session(fake_client, schema)
    assert fake_client.mutations == []
