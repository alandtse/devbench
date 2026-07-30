"""/api/health — off-thread liveness + instance identity (devbench#56, #16).

Unlike the tool endpoints, /api/health is answered on the listener thread with no
RunAndWait, so it must return even while the main thread is busy. These tests only
assert shape + identity parity (they can't force a main-thread stall without a game).
"""

from __future__ import annotations

import requests

from conftest import CALL_TIMEOUT


def _health(base_url: str) -> dict:
    resp = requests.get(f"{base_url}/api/health", timeout=CALL_TIMEOUT)
    assert resp.status_code == 200, resp.status_code
    body = resp.json()
    assert isinstance(body, dict), body
    return body


def test_health_shape(base_url):
    body = _health(base_url)
    assert body.get("ok") is True, body
    assert body.get("lastLifecycle") is None or isinstance(body["lastLifecycle"], str), body

    # ints, and explicitly NOT bool: bool is an int subclass in Python, so `isinstance(x, int)`
    # alone would pass a regression that serialized these as true/false.
    for key in ("frame", "lastTaskFrame", "pendingTasks", "pid", "port"):
        val = body.get(key)
        assert isinstance(val, int) and not isinstance(val, bool), (key, val)

    assert body["pid"] > 0, body
    assert 1 <= body["port"] <= 65535, body
    assert body["pendingTasks"] >= 0, body
    assert isinstance(body.get("exe"), str) and body["exe"].lower().endswith(".exe"), body
    assert isinstance(body.get("vr"), bool), body


def test_health_identity_matches_inspect(base_url, client):
    """/api/health and inspect{kind:state} must report the same instance identity —
    they share InstanceIdentity(), so a drift here means the two paths diverged."""
    health = _health(base_url)
    state = client.ok("inspect", {"kind": "state"})
    for key in ("pid", "port", "exe", "vr"):
        assert health.get(key) == state.get(key), (key, health.get(key), state.get(key))


def test_inspect_health_kind_mirrors_rest(base_url, client):
    """inspect{kind:health} is the MCP-reachable, off-thread liveness kind — it must mirror
    GET /api/health's liveness+identity core. lastLifecycle stays REST-only (MCP gets
    lifecycle via push), so it must NOT appear here."""
    rest = _health(base_url)
    kind = client.ok("inspect", {"kind": "health"})
    for key in ("pid", "port", "exe", "vr"):
        assert kind.get(key) == rest.get(key), (key, kind.get(key), rest.get(key))
    for key in ("frame", "lastTaskFrame", "pendingTasks"):
        assert isinstance(kind.get(key), int) and not isinstance(kind[key], bool), (key, kind.get(key))
    assert kind["pendingTasks"] >= 0, kind
    assert "lastLifecycle" not in kind, "inspect health is MCP-facing; lastLifecycle is REST-only"
