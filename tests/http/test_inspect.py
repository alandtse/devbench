"""Tests for the `inspect` tool (live game-state reads)."""

from __future__ import annotations

import numbers

import pytest

from conftest import require_enum, require_tool


def _is_number(v) -> bool:
    return isinstance(v, numbers.Number) and not isinstance(v, bool)


@pytest.fixture
def inspect(tool_schema):
    return require_tool(tool_schema, "inspect")


def test_state(client, inspect):
    require_enum(inspect, "kind", "state")
    body = client.ok("inspect", {"kind": "state"})
    assert isinstance(body, dict), body
    assert body.get("plugin") == "devbench", body
    assert "version" in body, body
    assert isinstance(body.get("vr"), bool), body
    assert isinstance(body.get("playerLoaded"), bool), body
    assert "frame" in body, body


def test_vm(client, inspect):
    require_enum(inspect, "kind", "vm")
    body = client.ok("inspect", {"kind": "vm"})
    assert body.get("available") is True, body
    assert isinstance(body.get("loadedTypes"), int) and body["loadedTypes"] > 0, body
    for field in ("attachedScripts", "arrays", "runningStacks", "frozenStacks"):
        assert isinstance(body.get(field), int), (field, body)
    assert isinstance(body.get("overstressed"), bool), body


@pytest.mark.requires_player
def test_scene(client, inspect):
    require_enum(inspect, "kind", "scene")
    body = client.ok("inspect", {"kind": "scene"})
    assert body.get("playerLoaded") is True, body

    cell = body.get("cell")
    assert isinstance(cell, dict), body
    assert "formId" in cell and "formType" in cell and "name" in cell, cell

    pos = body.get("position")
    assert isinstance(pos, list) and len(pos) == 3, body
    assert all(_is_number(c) for c in pos), pos

    hour = body.get("gameHour")
    assert _is_number(hour) and 0 <= hour < 24, body

    assert isinstance(body.get("weather"), dict), body


@pytest.mark.requires_player
def test_refs_player_by_formid(client, inspect):
    require_enum(inspect, "kind", "refs")
    body = client.ok("inspect", {"kind": "refs", "formId": "0x14"})
    assert body.get("count") == 1, body
    refs = body.get("refs")
    assert isinstance(refs, list) and len(refs) == 1, body
    ref = refs[0]
    assert ref.get("formId") == "0x00000014", ref
    assert ref.get("base", {}).get("formType") == "NPC_", ref
    actor = ref.get("actor")
    assert isinstance(actor, dict), ref
    assert _is_number(actor.get("health")), actor
    assert _is_number(actor.get("level")), actor


@pytest.mark.requires_player
def test_refs_actors_in_radius(client, inspect):
    require_enum(inspect, "kind", "refs")
    body = client.ok(
        "inspect",
        {"kind": "refs", "formType": "Actor", "radius": 100000, "limit": 200},
    )
    assert isinstance(body.get("count"), int) and body["count"] >= 1, body
    refs = body.get("refs")
    assert isinstance(refs, list) and refs, body

    # Every ref that is actually an actor must carry an `actor` snapshot.
    for ref in refs:
        ftype = (ref.get("formType") or "")
        btype = (ref.get("base", {}) or {}).get("formType", "")
        if "ACHR" in ftype or "ACHR" in btype or "NPC_" in btype:
            assert isinstance(ref.get("actor"), dict), ref

    assert any((r.get("base", {}) or {}).get("name") for r in refs), \
        "expected at least one actor ref with a base.name"


@pytest.mark.requires_player
def test_refs_enumerate(client, inspect):
    require_enum(inspect, "kind", "refs")
    body = client.ok("inspect", {"kind": "refs"})
    assert isinstance(body.get("count"), int) and body["count"] > 0, body
    assert isinstance(body.get("refs"), list), body
    assert isinstance(body.get("truncated"), bool), body


def test_mods(client, inspect):
    require_enum(inspect, "kind", "mods")
    body = client.ok("inspect", {"kind": "mods"})
    assert isinstance(body, dict), body
    for field in ("count", "lightCount", "total"):
        assert isinstance(body.get(field), int), (field, body)
    assert body["total"] == body["count"] + body["lightCount"], body

    plugins = body.get("plugins")
    light = body.get("lightPlugins")
    assert isinstance(plugins, list) and isinstance(light, list), body
    assert len(plugins) == body["count"], body
    assert len(light) == body["lightCount"], body

    # Skyrim.esm is always present (full plugin, load-order index 0) once data is loaded.
    for p in plugins:
        assert isinstance(p.get("index"), int), p
        assert isinstance(p.get("name"), str) and p["name"], p
    names = [p["name"].lower() for p in plugins]
    assert "skyrim.esm" in names, names


def test_extensions_list_and_dispatch(client, inspect):
    # RegisterToolExtension lets a mod add a custom inspect kind. The host registers a
    # `devbench.selftest` inspect extension through the public C-ABI (the ping pattern),
    # so `kind=extensions` lists it and `kind=devbench.selftest` routes to it and echoes.
    require_enum(inspect, "kind", "extensions")
    listed = client.ok("inspect", {"kind": "extensions"})
    exts = listed.get("extensions")
    assert isinstance(exts, list), listed
    kinds = [e.get("kind") for e in exts]
    if "devbench.selftest" not in kinds:
        pytest.skip("devbench.selftest inspect extension not registered on this build")
    assert isinstance(next(e for e in exts if e["kind"] == "devbench.selftest").get("descriptor"), dict), exts

    body = client.ok("inspect", {"kind": "devbench.selftest", "foo": 7})
    assert body.get("invoked") is True, body
    assert body.get("echo", {}).get("foo") == 7, body


def test_unknown_kind_400(client, inspect):
    require_enum(inspect, "kind", "extensions")  # feature present → unknown kind is a clean 400
    status, body = client.call("inspect", {"kind": "NoSuchKindXYZ"})
    assert status == 400, (status, body)
    assert isinstance(body, dict) and "error" in body, body
