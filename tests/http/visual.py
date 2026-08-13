"""Batch/corpus visual-regression scoring for devbench capture checkpoints.

devbench ALSO has a native, single-checkpoint SSIM verdict now (the `capture`
tool's `golden`/`threshold`/`regions` args, normally supplied via
`record{action:"replay", goldens:{...}}`) — that is the primary, mod-author-
facing way to get "did this checkpoint match" as {ssim, threshold, passed}
inline in the SAME HTTP/MCP call that ran the replay, no Python required. This
module is NOT that interface. It exists for the different job of scoring many
recordings/checkpoints at once, generating reports, and managing a `goldens/`
tree with `--visual-update` — genuine batch/corpus work, not a single
checkpoint's fast pass/fail. Reach for this when you're comfortable with Python
and want that; otherwise use the native `capture`/`record` args directly.

This module is pure: it never talks to the devbench server, only to PNG files
on disk, so it's unit-testable against static fixtures with no game running
(see test_visual.py). Its SSIM implementation (scikit-image, Gaussian-weighted
sliding window) is independent of devbench's own native C++ implementation
(uniform-weighted overlapping window) — the two are NOT guaranteed bit-exact
against each other, by design; each is suited to its own use case.

Layout convention (not enforced elsewhere — just what this module expects):

    tests/http/goldens/<recording>/<variant>/<checkpointId>.png   golden reference
    tests/http/goldens/<recording>/thresholds.json                per-checkpoint config

thresholds.json shape (all keys optional):

    {
      "_default": {"threshold": 0.98},
      "<checkpointId>": {
        "threshold": 0.98,
        "regions": [
          {"name": "water", "x": 0.0, "y": 0.4, "w": 1.0, "h": 0.3, "threshold": 0.90}
        ]
      }
    }

`regions` are optional per-checkpoint ROI overrides in 0..1 UV — the same
convention the `capture` tool's own `subrect` argument uses — scored
independently so a noisy area (particles, weather) can carry a looser threshold
than the frame as a whole, instead of loosening the whole-frame threshold to
tolerate it.
"""

from __future__ import annotations

import json
import shutil
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image
from skimage.metrics import structural_similarity as ssim

DEFAULT_THRESHOLD = 0.98


@dataclass
class RegionScore:
    name: str
    score: float
    threshold: float
    passed: bool


@dataclass
class CheckpointScore:
    checkpoint_id: str
    score: float  # whole-frame SSIM, or the worst region's score when regions are configured
    threshold: float
    passed: bool
    regions: list[RegionScore] = field(default_factory=list)
    # Set (to a human-readable reason) whenever `passed` should NOT be treated as a
    # real verdict — e.g. the capture was stamped sceneMismatch/degraded, or there's
    # no golden yet. Callers must check this before trusting `passed`.
    inconclusive_reason: str | None = None


def load_thresholds(recording_dir: Path) -> dict[str, Any]:
    """Read <recording_dir>/thresholds.json, or {} if absent (all-defaults)."""
    path = recording_dir / "thresholds.json"
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def _checkpoint_config(thresholds: dict[str, Any], checkpoint_id: str) -> dict[str, Any]:
    cfg: dict[str, Any] = dict(thresholds.get("_default", {}))
    cfg.update(thresholds.get(checkpoint_id, {}))
    cfg.setdefault("threshold", DEFAULT_THRESHOLD)
    return cfg


def _load_gray(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("L"), dtype=np.float64)


def _crop_uv(arr: np.ndarray, uv: dict[str, float]) -> np.ndarray:
    """Crop a 0..1 UV rect {x,y,w,h} — the same convention the capture tool's
    `subrect` argument uses, so a region here can be copy-pasted from there."""
    h, w = arr.shape[:2]
    x0 = int(round(uv.get("x", 0.0) * w))
    y0 = int(round(uv.get("y", 0.0) * h))
    x1 = int(round((uv.get("x", 0.0) + uv.get("w", 1.0)) * w))
    y1 = int(round((uv.get("y", 0.0) + uv.get("h", 1.0)) * h))
    return arr[max(0, y0):min(h, y1), max(0, x0):min(w, x1)]


def _ssim(a: np.ndarray, b: np.ndarray) -> float:
    if a.shape != b.shape:
        # A resolution/crop mismatch is a setup error (wrong golden, resized capture
        # window), not a shader regression — the caller should not report this as a
        # normal fail without noting why.
        raise ValueError(f"shape mismatch: candidate {a.shape} vs golden {b.shape}")
    if a.size == 0:
        raise ValueError("empty region")
    # skimage's default 7x7 window doesn't fit a smaller region (e.g. a tight ROI
    # crop) — shrink it (odd, >=3) rather than let structural_similarity raise.
    shortest = min(a.shape[0], a.shape[1])
    win = min(7, shortest if shortest % 2 else shortest - 1)
    win = max(3, win)
    return float(ssim(a, b, data_range=255.0, win_size=win))


def score_checkpoint(
    candidate_path: Path,
    golden_path: Path,
    checkpoint_id: str,
    thresholds: dict[str, Any],
    *,
    inconclusive_reason: str | None = None,
) -> CheckpointScore:
    """Score one checkpoint's candidate PNG against its golden.

    Never raises for a genuine visual mismatch — that's an ordinary
    `passed=False` result. Only raises for setup errors (unreadable image,
    shape mismatch); a missing golden is reported as inconclusive, not raised,
    since "no golden yet" is an expected first-run state, not a bug.
    """
    cfg = _checkpoint_config(thresholds, checkpoint_id)
    threshold = float(cfg["threshold"])

    if not golden_path.is_file():
        return CheckpointScore(
            checkpoint_id=checkpoint_id, score=0.0, threshold=threshold, passed=False,
            inconclusive_reason=inconclusive_reason or f"no golden at {golden_path}",
        )

    cand = _load_gray(candidate_path)
    gold = _load_gray(golden_path)

    region_cfgs = cfg.get("regions", [])
    if region_cfgs:
        regions = []
        for r in region_cfgs:
            r_score = _ssim(_crop_uv(cand, r), _crop_uv(gold, r))
            r_threshold = float(r.get("threshold", threshold))
            regions.append(RegionScore(
                name=r.get("name", "?"), score=r_score,
                threshold=r_threshold, passed=r_score >= r_threshold,
            ))
        return CheckpointScore(
            checkpoint_id=checkpoint_id, score=min(r.score for r in regions), threshold=threshold,
            passed=all(r.passed for r in regions), regions=regions,
            inconclusive_reason=inconclusive_reason,
        )

    score = _ssim(cand, gold)
    return CheckpointScore(
        checkpoint_id=checkpoint_id, score=score, threshold=threshold,
        passed=score >= threshold, inconclusive_reason=inconclusive_reason,
    )


def promote_golden(candidate_path: Path, golden_path: Path) -> None:
    """Copy a candidate over its golden — the `--visual-update` path. Callers
    decide when this is appropriate (a reviewed, intentional shader change)."""
    golden_path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(candidate_path, golden_path)


def capture_inconclusive_reason(capture_result: dict[str, Any]) -> str | None:
    """Why a `capture` tool result should NOT be scored as a real pass/fail, or
    None if it's trustworthy. Mirrors the rules in
    docs/plans/replay-checkpoint-capture.md Part 4: a mismatched/unsettled scene,
    any `degraded` stamp (native fallback, UI included, etc.), or a non-provider
    capture are all reported as inconclusive rather than a false regression."""
    if capture_result.get("sceneMismatch"):
        return "sceneMismatch"
    degraded = capture_result.get("degraded") or []
    if degraded:
        return f"degraded: {', '.join(degraded)}"
    if capture_result.get("provider") == "native":
        return "captured via the low-fidelity native fallback, not a registered provider"
    return None
