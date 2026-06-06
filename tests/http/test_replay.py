"""Replay coupling: consumer override of the producer's coupling signal.

A recipe's `meta.coupling.tier` is the producer's signal; a consumer can override
it on `record action=replay` — `coupling` forces a looser tier (worldspace skips
the scene restore) and `force` turns a scene mismatch from an abort into a
reported warning. These tests write a tiny throwaway recording and replay it.

Skip-safe: needs the `record` tool with the `coupling` arg (feature present), a
loaded player, and a server that can read the local recording path (skips on a
remote server).
"""

from __future__ import annotations

import json
import os
import tempfile

import pytest

from conftest import require_enum, require_tool


def _write_recording(meta: dict) -> str:
    fd, path = tempfile.mkstemp(suffix=".json", prefix="devbench_rec_")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump({"meta": meta, "steps": [{"wait": 100}]}, f)
    return path


@pytest.fixture
def record(tool_schema):
    rec = require_tool(tool_schema, "record")
    require_enum(rec, "coupling", "worldspace")  # skip on builds without the override
    return rec


@pytest.mark.requires_player
def test_replay_consumer_overrides_producer_coupling(client, record):
    # Producer signals "anchored" with a save entry that would fail to load; the
    # consumer forces "worldspace" → the restore is skipped entirely, and the result
    # reports the override. cellFormID 1 never matches, but force makes the mismatch
    # a warning, not an abort — so the run still succeeds.
    path = _write_recording({
        "interior": True, "cellFormID": 1, "cell": "Nowhere",
        "coupling": {"tier": "anchored"},
        "entryPoint": {"kind": "save", "value": "devbench_no_such_save", "ageMs": 0},
    })
    try:
        status, body = client.call(
            "record",
            {"action": "replay", "path": path, "restoreScene": True,
             "coupling": "worldspace", "force": True},
            timeout=60.0,
        )
    finally:
        os.unlink(path)

    if status == 404:
        pytest.skip("server can't read the local recording path (remote server)")
    assert status == 200, body

    coupling = body.get("coupling", {})
    assert coupling.get("tier") == "worldspace", body          # consumer's choice
    assert coupling.get("producer") == "anchored", body        # producer's signal preserved
    assert coupling.get("overridden") is True, body
    assert coupling.get("forced") is True, body

    # forced → the scene mismatch is reported, not fatal.
    assert body.get("ok") is True, body
    scene = [r for r in body.get("results", []) if r.get("assert") == "scene"]
    assert scene and scene[0].get("sceneMismatch") is True, body


@pytest.mark.requires_player
def test_replay_scene_mismatch_aborts_without_force(client, record):
    # Same mismatch, but without force the scene assert aborts the replay (the
    # default contract — a wrong scene shouldn't silently run).
    path = _write_recording({
        "interior": True, "cellFormID": 1, "cell": "Nowhere",
        "coupling": {"tier": "worldspace"},
        "entryPoint": {"kind": "coc", "value": "Nowhere", "ageMs": 0},
    })
    try:
        status, body = client.call(
            "record", {"action": "replay", "path": path, "coupling": "worldspace"},
            timeout=60.0,
        )
    finally:
        os.unlink(path)

    if status == 404:
        pytest.skip("server can't read the local recording path (remote server)")
    assert status == 200, body
    assert body.get("ok") is False and body.get("aborted") is True, body
    scene = [r for r in body.get("results", []) if r.get("assert") == "scene"]
    assert scene and scene[0].get("errorCode") == 409, body
