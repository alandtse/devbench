# Default recordings

Curated, validated test replays that ship with devbench, for reproducible A/B benchmarking
(replay the same player trajectory against each build under test). `index.json` lists them.

## Using one

```
record action=replay path=default_recordings/GuardianStonesToWhiterun.json restoreScene=true
```

Replay re-establishes the recorded entry (here `coc GuardianStones`), then drives the exact
same pose path. Because the entry is a `coc` and the poses are world coordinates, a flat
recording is comparable across **SE and AE**; it is **not** VR-comparable (VR drives pitch and
culling from the HMD), so `meta.runtime.compat` gates replay per runtime — a VR benchmark needs
its own VR-recorded recording. Pass `force` to replay across the gate anyway.

## Conventions

- **Naming:** `<StartZone>To<EndZone>[_variant].json` (e.g. `GuardianStonesToWhiterun.json`).
- **Format:** `devbench-recording-2` — one compact `{ "pose": [x,y,z,yawDeg,pitchDeg], "wait": ms }`
  step per sample (hand-editable, one line each). See the record tool docs for the schema.
- **`index.json`:** add an entry (`name`, `file`, `route`, `entry`, `runtime`, `validated`, …)
  when you add a recording. Mark `validated: true` only after confirming a clean replay.
- Keep the shipped set small; host a larger library as a separate repo or release asset.
