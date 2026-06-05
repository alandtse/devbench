#!/usr/bin/env python3
"""Example: drive and monitor a 3-way combat scene through devbench's REST API.

This is an *example recipe*, not a CI test — it mutates game state heavily and its
outcome is non-deterministic, so it lives outside the pytest suite. Run it by hand
against a running Skyrim with an in-world save already loaded:

    python tests/http/examples/combat_arena.py

It spawns three mutually-hostile factions on the player, enables god mode so the
player can't die, and reports live per-team standings until the window elapses.

Why it is written this way (Skyrim-engine constraints learned the hard way):

  * Combat AI only runs in the PLAYER's high-process zone. Spawn combatants ON the
    player (`player.placeatme`) — actors spawned/teleported far away just stand
    idle at full health.
  * Only PLAYER-prefixed console commands work through devbench's command path
    (`player.placeatme`, `player.moveto`). `<ref>.cmd` and `prid <ref>; cmd`
    silently no-op (programmatic injection doesn't keep the console's selected
    ref between commands). Form IDs in console commands must be BARE hex (`9F358`).
  * Read/measure via papyrus member calls — they work regardless of distance:
    `Actor.GetActorValue("Health")`, `Actor.IsDead`. AV-mutating calls
    (`DamageActorValue`, `SetActorValue`) also apply; reference-moving ones
    (`MoveTo`, `SetPosition`) currently do not, so this recipe doesn't rely on
    them. If you DO call a latent papyrus function, pass `"async": true` so it
    returns when issued instead of stalling the request on its late callback.
  * Bandits (`0x0009F358`) are hostile to everyone, so they always engage and
    produce a clean, monitored elimination. Civil-war soldiers' mutual hostility
    is quest-state dependent, so a full "last team standing" finish isn't
    guaranteed — the goal here is to exercise spawn + combat + live monitoring.
"""
from __future__ import annotations

import os
import sys
import time

import requests

# Three mutually-hostile vanilla bases (bare hex, as the console wants).
TEAMS = {
    "Bandits": "9F358",       # 0x0009F358 — hostile to all
    "Imperials": "1713E",     # 0x0001713E
    "Stormcloaks": "17167",   # 0x00017167
}
BASE_NAME = {"Bandit": "Bandits", "Imperial Soldier": "Imperials", "Stormcloak Soldier": "Stormcloaks"}
PER_TEAM = 8
WINDOW_S = 70


def discover() -> str:
    """DEVBENCH_URL, else probe the default port range for a live server."""
    env = os.environ.get("DEVBENCH_URL")
    candidates = [env] if env else [f"http://127.0.0.1:{p}" for p in range(8920, 8926)]
    for url in candidates:
        try:
            r = requests.post(f"{url}/api/tool/inspect", json={"kind": "state"}, timeout=4)
            if r.ok and isinstance(r.json(), dict) and "plugin" in r.json():
                return url
        except requests.RequestException:
            continue
    sys.exit("no reachable devbench server (start Skyrim with the plugin, or set DEVBENCH_URL)")


URL = discover()


def console(cmd: str) -> None:
    requests.post(f"{URL}/api/tool/console", json={"command": cmd}, timeout=8)


def papyrus(fn: str, self_form: str, args=None, script="Actor"):
    body = {"action": "call", "script": script, "function": fn, "self": {"form": self_form}}
    if args is not None:
        body["args"] = args
    try:
        return requests.post(f"{URL}/api/tool/papyrus", json=body, timeout=10).json()
    except requests.RequestException:
        return {}


def actors():
    r = requests.post(f"{URL}/api/tool/inspect",
                      json={"kind": "refs", "formType": "Actor", "limit": 150}, timeout=15)
    return r.json().get("refs", [])


def health(form: str):
    v = papyrus("GetActorValue", form, ["Health"]).get("returned")
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def alive(form: str) -> bool:
    if papyrus("IsDead", form).get("returned"):
        return False
    h = health(form)
    return h is not None and h > 0


def main() -> None:
    if not requests.post(f"{URL}/api/tool/inspect", json={"kind": "state"}, timeout=5).json().get("playerLoaded"):
        sys.exit("no in-world save loaded — load a save first")

    print(f"server: {URL}")
    print(f"tgm + spawn {PER_TEAM}/team on the player")
    console("tgm")
    before = {a["formId"] for a in actors()}
    for base in TEAMS.values():
        console(f"player.placeatme {base} {PER_TEAM} 1")
        time.sleep(1)
    time.sleep(2)

    teams = {t: [] for t in TEAMS}
    for r in actors():
        if r["formId"] in before:
            continue
        fac = BASE_NAME.get(r.get("base", {}).get("name"))
        if fac:
            teams[fac].append(r["formId"])
    counts = {t: len(v) for t, v in teams.items()}
    print("spawned:", counts)
    if sum(counts.values()) < 3:
        sys.exit("spawn failed (no combatants found)")

    print("=== observing ===")
    end = time.time() + WINDOW_S
    tick = 0
    eliminated: list[str] = []
    snap = teams
    while time.time() < end:
        snap = {t: [f for f in forms if alive(f)] for t, forms in teams.items()}
        for t in teams:
            if counts[t] and not snap[t] and t not in eliminated:
                eliminated.append(t)
                print(f"  ** {t} eliminated at t={tick}s **")
        print(f"t={tick:>3}s  " + " | ".join(f"{t}:{len(snap[t])}" for t in teams))
        if sum(1 for t in teams if snap[t]) <= 1:
            break
        time.sleep(2)
        tick += 2

    print("=== standings ===")
    ranked = sorted(teams, key=lambda t: len(snap[t]), reverse=True)
    for rank, t in enumerate(ranked, 1):
        print(f"  {rank}. {t}: {len(snap[t])}/{counts[t]} standing")
    print(f"leader: {ranked[0]}"
          + (f"; eliminated: {', '.join(eliminated)}" if eliminated else ""))


if __name__ == "__main__":
    main()
