"""Unit tests for visual.py's SSIM/threshold/ROI scoring.

Pure — no devbench server, no game, no Skyrim install needed. Synthesizes tiny
PNGs with Pillow and scores them directly, so this validates the scoring math
itself independent of the (not-yet-live) capture pipeline it will eventually
score real output from.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
from PIL import Image

from visual import capture_inconclusive_reason, promote_golden, score_checkpoint


def _write_png(path: Path, pixels: np.ndarray) -> None:
    Image.fromarray(pixels.astype(np.uint8), mode="L").save(path)


def _solid(size: tuple[int, int], value: int) -> np.ndarray:
    w, h = size
    return np.full((h, w), value, dtype=np.uint8)


def _checker(size: tuple[int, int], a: int, b: int, block: int = 4) -> np.ndarray:
    w, h = size
    yy, xx = np.mgrid[0:h, 0:w]
    return np.where(((xx // block) + (yy // block)) % 2 == 0, a, b).astype(np.uint8)


def test_identical_images_score_near_one(tmp_path: Path) -> None:
    img = _checker((32, 32), 60, 200)
    cand = tmp_path / "cand.png"
    gold = tmp_path / "gold.png"
    _write_png(cand, img)
    _write_png(gold, img)

    result = score_checkpoint(cand, gold, "cp1", {})
    assert result.passed is True
    assert result.score > 0.99, result
    assert result.inconclusive_reason is None


def test_very_different_images_fail(tmp_path: Path) -> None:
    cand = tmp_path / "cand.png"
    gold = tmp_path / "gold.png"
    _write_png(cand, _solid((32, 32), 0))
    _write_png(gold, _solid((32, 32), 255))

    result = score_checkpoint(cand, gold, "cp1", {})
    assert result.passed is False
    assert result.score < 0.5, result


def test_custom_threshold_from_config(tmp_path: Path) -> None:
    img = _checker((32, 32), 100, 150)
    noisy = img.copy()
    noisy[0, 0] = 255  # a tiny, deliberate perturbation
    cand, gold = tmp_path / "cand.png", tmp_path / "gold.png"
    _write_png(cand, noisy)
    _write_png(gold, img)

    strict = score_checkpoint(cand, gold, "cp1", {"cp1": {"threshold": 0.999999}})
    lenient = score_checkpoint(cand, gold, "cp1", {"cp1": {"threshold": 0.0}})
    assert strict.passed is False
    assert lenient.passed is True
    assert strict.score == lenient.score  # same measurement, different gate


def test_default_threshold_applies_when_checkpoint_unlisted(tmp_path: Path) -> None:
    img = _checker((32, 32), 60, 200)
    cand, gold = tmp_path / "cand.png", tmp_path / "gold.png"
    _write_png(cand, img)
    _write_png(gold, img)

    result = score_checkpoint(cand, gold, "unlisted_checkpoint", {"_default": {"threshold": 0.5}})
    assert result.threshold == 0.5


def test_missing_golden_is_inconclusive_not_a_crash(tmp_path: Path) -> None:
    cand = tmp_path / "cand.png"
    _write_png(cand, _solid((16, 16), 128))

    result = score_checkpoint(cand, tmp_path / "no_such_golden.png", "cp1", {})
    assert result.passed is False
    assert result.inconclusive_reason is not None
    assert "no golden" in result.inconclusive_reason


def test_shape_mismatch_raises_as_a_setup_error(tmp_path: Path) -> None:
    cand, gold = tmp_path / "cand.png", tmp_path / "gold.png"
    _write_png(cand, _solid((16, 16), 128))
    _write_png(gold, _solid((32, 32), 128))

    with pytest.raises(ValueError, match="shape mismatch"):
        score_checkpoint(cand, gold, "cp1", {})


def test_region_scoring_is_independent_per_region(tmp_path: Path) -> None:
    # Left half identical, right half wildly different.
    base = _checker((32, 32), 60, 200)
    cand = base.copy()
    cand[:, 16:] = 255 - cand[:, 16:]
    cand_path, gold_path = tmp_path / "cand.png", tmp_path / "gold.png"
    _write_png(cand_path, cand)
    _write_png(gold_path, base)

    thresholds = {
        "cp1": {
            "threshold": 0.9,
            "regions": [
                {"name": "left", "x": 0.0, "y": 0.0, "w": 0.5, "h": 1.0, "threshold": 0.9},
                {"name": "right", "x": 0.5, "y": 0.0, "w": 0.5, "h": 1.0, "threshold": 0.9},
            ],
        }
    }
    result = score_checkpoint(cand_path, gold_path, "cp1", thresholds)
    assert len(result.regions) == 2
    left = next(r for r in result.regions if r.name == "left")
    right = next(r for r in result.regions if r.name == "right")
    assert left.passed is True, left
    assert right.passed is False, right
    assert result.passed is False  # overall fails if ANY region fails
    assert result.score == right.score  # overall score is the worst region


def test_region_with_looser_threshold_can_still_pass(tmp_path: Path) -> None:
    base = _checker((32, 32), 60, 200)
    cand = base.copy()
    cand[:, 16:] = 255 - cand[:, 16:]  # right half noisy — e.g. weather/particles
    cand_path, gold_path = tmp_path / "cand.png", tmp_path / "gold.png"
    _write_png(cand_path, cand)
    _write_png(gold_path, base)

    # SSIM ranges -1..1, not 0..1 (a fully-inverted region scores strongly
    # NEGATIVE, not merely low) — a "loose" threshold that should accept
    # anything must reach down to -1.0, not 0.0.
    thresholds = {
        "cp1": {
            "threshold": 0.9,
            "regions": [
                {"name": "left", "x": 0.0, "y": 0.0, "w": 0.5, "h": 1.0, "threshold": 0.9},
                {"name": "right", "x": 0.5, "y": 0.0, "w": 0.5, "h": 1.0, "threshold": -1.0},
            ],
        }
    }
    result = score_checkpoint(cand_path, gold_path, "cp1", thresholds)
    assert result.passed is True


def test_promote_golden_copies_candidate_over_golden(tmp_path: Path) -> None:
    cand = tmp_path / "cand.png"
    golden = tmp_path / "goldens" / "rec" / "default" / "cp1.png"
    _write_png(cand, _solid((8, 8), 42))
    assert not golden.exists()

    promote_golden(cand, golden)

    assert golden.is_file()
    result = score_checkpoint(cand, golden, "cp1", {})
    assert result.passed is True
    assert result.inconclusive_reason is None


@pytest.mark.parametrize(
    ("capture_result", "expect_reason_contains"),
    [
        ({"sceneMismatch": True}, "sceneMismatch"),
        ({"degraded": ["nativeFallback"]}, "nativeFallback"),
        ({"provider": "native"}, "native fallback"),
        ({"provider": "openshaders", "sceneMismatch": False, "degraded": []}, None),
    ],
)
def test_capture_inconclusive_reason(capture_result, expect_reason_contains) -> None:
    reason = capture_inconclusive_reason(capture_result)
    if expect_reason_contains is None:
        assert reason is None
    else:
        assert reason is not None and expect_reason_contains in reason
