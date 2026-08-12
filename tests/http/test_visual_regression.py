"""Live shader-checkpoint visual regression: replay a recording with
meta.checkpoints, capture the frames, score each against its golden.

All comparison logic lives in visual.py (pure, server-independent — see its unit
tests in test_visual.py); this module is only the live-server plumbing: run the
replay, walk the transcript for `capture` steps, and either assert against
goldens or (with --visual-update) promote the captures to be the new goldens.

Skip-safe:
  - No `capture` tool in the live schema -> skip (older build).
  - No registered capture provider AND the recording doesn't allow native ->
    skip (nothing would produce a comparable image).
  - Server can't read the local temp recording path (remote server) -> skip.
  - A capture whose result is inconclusive (scene mismatch, degraded, native
    fallback) is reported as inconclusive, NEVER as a failure — see
    visual.capture_inconclusive_reason.
"""

from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path
from typing import Any

import pytest

from conftest import require_tool
from visual import capture_inconclusive_reason, load_thresholds, promote_golden, score_checkpoint

GOLDENS_DIR = Path(__file__).parent / "goldens"

# A minimal one-checkpoint recording: no trajectory beyond a single wait, so the
# checkpoint (anchored at atMs=0) fires almost immediately. Good enough to
# exercise the capture step's plumbing without needing a real recorded session.
_RECORDING_NAME = "visual_smoke"
_CHECKPOINT_ID = "smoke"


def _write_recording(*, variant: str, allow_native: bool) -> str:
    meta: dict[str, Any] = {
        "checkpoints": [{"id": _CHECKPOINT_ID, "atMs": 0, "excludeUi": True}],
    }
    if allow_native:
        meta["capabilities"] = [
            {"capability": "capture", "required": True, "allowNative": True}
        ]
    fd, path = tempfile.mkstemp(suffix=".json", prefix=f"devbench_{_RECORDING_NAME}_")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump({"meta": meta, "steps": [{"wait": 50}]}, f)
    return path


@pytest.fixture
def capture(tool_schema):
    return require_tool(tool_schema, "capture")


@pytest.fixture
def record(tool_schema):
    return require_tool(tool_schema, "record")


def _capture_provider_registered(client) -> bool:
    status, body = client.call("inspect", {"kind": "registrants"})
    if status != 200 or not isinstance(body, dict):
        return False
    return bool(body.get("capabilities", {}).get("capture"))


@pytest.mark.requires_player
def test_checkpoint_capture_and_score(client, capture, record, request: pytest.FixtureRequest):
    allow_native = not _capture_provider_registered(client)
    if allow_native:
        pytest.skip(
            "no capture provider registered (see inspect kind=registrants) — "
            "run with a provider mod loaded (e.g. Open Shaders) for a comparable image, "
            "or rerun this test after a provider is present"
        )

    path = _write_recording(variant="default", allow_native=allow_native)
    try:
        status, body = client.call(
            "record",
            {"action": "replay", "path": path, "variant": "default", "async": False},
            timeout=60.0,
        )
    finally:
        os.unlink(path)

    if status == 404:
        pytest.skip("server can't read the local recording path (remote server)")
    assert status == 200, body

    capture_results = [
        r["result"] for r in body.get("results", [])
        if r.get("kind") == "tool" and r.get("tool") == "capture" and r.get("ok") and "result" in r
    ]
    assert capture_results, f"no successful capture step in replay transcript: {body}"
    result = capture_results[0]
    assert result.get("checkpointId") == _CHECKPOINT_ID, result

    reason = capture_inconclusive_reason(result)
    if reason is not None:
        pytest.skip(f"capture is not comparable: {reason}")

    recording_dir = GOLDENS_DIR / _RECORDING_NAME
    golden_path = recording_dir / "default" / f"{_CHECKPOINT_ID}.png"
    candidate_path = Path(result["path"])

    if request.config.getoption("--visual-update"):
        promote_golden(candidate_path, golden_path)
        pytest.skip(f"--visual-update: promoted {candidate_path} -> {golden_path}")

    if not golden_path.is_file():
        pytest.skip(
            f"no golden yet at {golden_path} — run with --visual-update once to create it "
            "(review the image before committing it)"
        )

    thresholds = load_thresholds(recording_dir)
    score = score_checkpoint(candidate_path, golden_path, _CHECKPOINT_ID, thresholds)
    assert score.inconclusive_reason is None, score
    assert score.passed, (
        f"checkpoint '{_CHECKPOINT_ID}' SSIM {score.score:.4f} below threshold "
        f"{score.threshold:.4f} (candidate: {candidate_path}, golden: {golden_path})"
    )
