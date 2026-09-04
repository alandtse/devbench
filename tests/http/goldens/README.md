# Visual-regression goldens

Reference images for `test_visual_regression.py`, scored via `visual.py`
(SSIM). Never generated or scored by devbench itself — comparison is entirely
external, on purpose: devbench's job is reproducing a frame deterministically,
not judging whether it looks right.

## Layout

```
goldens/<recording>/thresholds.json           # optional, see below
goldens/<recording>/<variant>/<checkpointId>.png
```

`<recording>` is the recording file's stem, `<variant>` is whatever `record
action=replay`'s `variant` arg was (default `"default"`), `<checkpointId>`
matches a `meta.checkpoints[].id` in the recording.

## `thresholds.json`

Optional, per-recording. All keys optional:

```jsonc
{
  "_default": { "threshold": 0.98 },
  "<checkpointId>": {
    "threshold": 0.98,
    "regions": [
      {
        "name": "water",
        "x": 0.0,
        "y": 0.4,
        "w": 1.0,
        "h": 0.3,
        "threshold": 0.9,
      },
    ],
  },
}
```

`regions` are optional 0..1 UV crops (same convention as the `capture` tool's
`subrect` arg) scored independently — use this to give a noisy area (weather,
particles) a looser threshold instead of loosening the whole frame.

## Creating/updating a golden

```sh
pytest tests/http/test_visual_regression.py --visual-update
```

Review the resulting PNG diff before committing — this overwrites the golden
unconditionally, it doesn't ask.
