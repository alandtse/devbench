#!/usr/bin/env python3
"""Example: find a placed mesh/object and frame it for a screenshot.

An *example recipe*, not a CI test — it moves the player and mutates the
camera POV, so it lives outside the pytest suite. Run it by hand against a
running Skyrim with an in-world save already loaded:

    python tests/http/examples/goto_mesh.py --model wrcity01
    python tests/http/examples/goto_mesh.py --formid 0x1a3f2
    python tests/http/examples/goto_mesh.py --editorid SomeUniqueRef

Composes existing devbench tools, no new native code beyond the 'refs'
enumeration reporting 'model' (base object mesh path), 'bounds' and
'rotation' (see `inspect` tool docs):

  1. `inspect refs` — locate the target by mesh-path substring, FormID, or
     EditorID, and read its world position + bounding box.
  2. `console player.moveto` — teleport the player to a standoff point near
     the target. Moving the PLAYER (not a free camera) keeps this reliable
     across SE/VR: it sidesteps the free camera's internal rotation
     convention, which the `camera` tool docs call out as best-effort and
     in need of in-game tuning (see docs/vr-free-camera.md). Console
     MoveTo/SetPos only takes effect on PLAYER-prefixed commands.
  3. `papyrus call ObjectReference.GetHeadingAngle` — ask the engine for the
     exact heading from the player to the target, instead of hand-rolling
     bearing trigonometry.
  4. `papyrus call ObjectReference.SetAngle` — face the player at that
     heading, then `camera setPov` third-person so the target is framed in
     view, and `capture`.

Distance defaults to the target's bounding-box diagonal (so small props and
whole buildings both end up reasonably framed) plus a fixed margin; override
with --distance to tune per shot.
"""
from __future__ import annotations

import argparse
import math
import os
import sys
import time

import requests


def discover() -> str:
    env = os.environ.get("DEVBENCH_URL")
    for url in ([env] if env else [f"http://127.0.0.1:{p}" for p in range(8920, 8926)]):
        try:
            r = requests.post(f"{url}/api/tool/inspect", json={"kind": "state"}, timeout=4)
            if r.ok and isinstance(r.json(), dict) and "plugin" in r.json():
                return url
        except requests.RequestException:
            continue
    sys.exit("no reachable devbench server (start Skyrim with the plugin, or set DEVBENCH_URL)")


def tool(url: str, name: str, args: dict) -> dict:
    r = requests.post(f"{url}/api/tool/{name}", json=args, timeout=20)
    r.raise_for_status()
    return r.json()


def find_target(url: str, *, model: str | None, formid: str | None, editorid: str | None) -> dict:
    if formid or editorid:
        result = tool(url, "inspect", {"kind": "refs", "formId": formid or editorid})
    elif model:
        result = tool(url, "inspect", {"kind": "refs", "model": model, "limit": 1})
    else:
        sys.exit("pass one of --model / --formid / --editorid")
    refs = result.get("refs", [])
    if not refs:
        sys.exit(f"no matching ref found ({result})")
    return refs[0]


def standoff_distance(target: dict, override: float | None) -> float:
    if override is not None:
        return override
    bounds = target.get("bounds")
    if not bounds:
        return 350.0  # no bounds (e.g. some base types) — a reasonable default for architecture
    lo, hi = bounds["min"], bounds["max"]
    diag = math.dist(lo, hi)
    return max(200.0, diag * 1.5)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", help="substring of the base object's mesh (.nif) path")
    ap.add_argument("--formid", help="hex FormID, e.g. 0x1a3f2")
    ap.add_argument("--editorid", help="EditorID")
    ap.add_argument("--distance", type=float, default=None, help="standoff distance in game units (default: auto from bounds)")
    args = ap.parse_args()

    url = discover()
    if not tool(url, "inspect", {"kind": "state"}).get("playerLoaded"):
        sys.exit("no in-world save loaded — load a save first")

    target = find_target(url, model=args.model, formid=args.formid, editorid=args.editorid)
    pos = target["position"]
    print(f"target: {target.get('name') or target.get('editorId') or target['formId']} "
          f"({target.get('model', 'no mesh path')}) at {pos}")

    dist = standoff_distance(target, args.distance)
    # South of the target, roughly eye height above its footprint — a workable default framing
    # for exterior architecture; re-run with --distance to tighten/loosen the shot.
    standoff = [pos[0], pos[1] - dist, pos[2] + 64.0]
    print(f"standoff: {standoff} (distance={dist:.0f})")

    tool(url, "console", {"command": f"player.moveto {target['formId'].replace('0x', '')}"})
    tool(url, "console", {"command": f"player.setpos x {standoff[0]}"})
    tool(url, "console", {"command": f"player.setpos y {standoff[1]}"})
    tool(url, "console", {"command": f"player.setpos z {standoff[2]}"})
    time.sleep(0.5)  # let the cell finish loading around the new position before framing/capture

    heading = tool(url, "papyrus", {
        "action": "call", "script": "ObjectReference", "function": "GetHeadingAngle",
        "self": {"form": "0x14"}, "args": [{"form": target["formId"]}],
    }).get("returned")
    if heading is None:
        sys.exit(f"GetHeadingAngle failed: {heading}")
    tool(url, "papyrus", {
        "action": "call", "script": "ObjectReference", "function": "SetAngle",
        "self": {"form": "0x14"}, "args": [0.0, 0.0, heading],
    })
    tool(url, "camera", {"action": "setPov", "pov": "third"})
    time.sleep(0.5)  # let the camera settle into the new POV/facing before capture

    checkpoint = (args.model or args.formid or args.editorid or "target").replace("\\", "_").replace(".", "_")
    shot = tool(url, "capture", {"checkpointId": f"goto_mesh_{checkpoint}", "allowNative": True})
    print(f"captured: {shot}")


if __name__ == "__main__":
    main()
