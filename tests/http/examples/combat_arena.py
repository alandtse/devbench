#!/usr/bin/env python3
"""Example: drive, force, and resolve a 3-way combat scene through devbench's REST API.

An *example recipe*, not a CI test — it mutates game state heavily and runs for a
minute, so it lives outside the pytest suite. Run it by hand against a running
Skyrim with an in-world save already loaded:

    python tests/http/examples/combat_arena.py

It spawns three factions on the player, enables god mode, then each tick gathers
the survivors onto the player and forces the 3-way, reporting live per-team counts
until one team is left standing.

Why it is written this way (Skyrim-engine constraints learned the hard way):

  * Combat AI only runs in the PLAYER's high-process zone, and survivors scatter
    and de-aggro in a big cell. So each tick we MoveTo every living combatant onto
    the player (`ObjectReference.MoveTo`) — high-process zone + adjacency.
  * Faction hostility is unreliable: bandits are hostile to everyone, but civil-war
    Imperial vs Stormcloak soldiers don't auto-fight in a pre-war save. So we force
    it with `Actor.StartCombat` between the surviving teams' leads every tick.
  * Both of those rely on devbench padding omitted optional args: `MoveTo` and
    `StartCombat` each have optional params, and without the pad they dispatch but
    silently no-op. Needs a build with that fix (see the `papyrus` tool docs).
  * Only PLAYER-prefixed console commands take effect (`player.placeatme`); form
    IDs in console commands are BARE hex. Read/measure via papyrus member calls
    (`GetActorValue`, `IsDead`) — they work regardless of distance.
"""
from __future__ import annotations

import json
import os
import sys
import time

import requests

# Three vanilla bases (bare hex). Bandits are hostile to all; the soldiers are
# forced to fight via StartCombat.
TEAMS = {"Bandits": "9F358", "Imperials": "1713E", "Stormcloaks": "17167"}
BASE_NAME = {"Bandit": "Bandits", "Imperial Soldier": "Imperials", "Stormcloak Soldier": "Stormcloaks"}
PLAYER = "0x14"
PER_TEAM = 5
WINDOW_S = 240


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


URL = discover()


def console(cmd: str) -> None:
    try:
        requests.post(f"{URL}/api/tool/console", json={"command": cmd}, timeout=8)
    except requests.RequestException:
        pass


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


def alive(form: str) -> bool:
    # One call per actor keeps the per-tick poll cheap (this loop hits every combatant).
    return not papyrus("IsDead", form).get("returned")


def main() -> None:
    if not requests.post(f"{URL}/api/tool/inspect", json={"kind": "state"}, timeout=5).json().get("playerLoaded"):
        sys.exit("no in-world save loaded — load a save first")

    print(f"server: {URL}\ntgm + spawn {PER_TEAM}/team on the player")
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

    print("=== resolving (gather + force the 3-way) ===")
    end = time.time() + WINDOW_S
    tick = 0
    live = teams
    while time.time() < end:
        live = {t: [f for f in forms if alive(f)] for t, forms in teams.items()}
        standing = [t for t, v in live.items() if v]
        print(f"t={tick:>3}s  " + " | ".join(f"{t}:{len(live[t])}" for t in teams))
        if len(standing) <= 1:
            break
        for forms in live.values():           # gather survivors onto the player
            for f in forms:
                papyrus("MoveTo", f, [{"form": PLAYER}], script="ObjectReference")
        leads = {t: v[0] for t, v in live.items() if v}   # force the 3-way
        for a in leads:
            for b in leads:
                if a != b:
                    papyrus("StartCombat", leads[a], [{"form": leads[b]}])
        time.sleep(3)
        tick += 3

    print("=== result ===")
    standing = [t for t, v in live.items() if v]
    print(f"WINNER: {standing[0]}" if len(standing) == 1 else f"unresolved: {json.dumps({t: len(v) for t, v in live.items()})}")
    for t in teams:
        print(f"  {t}: {len(live[t])}/{counts[t]} survived")


if __name__ == "__main__":
    main()
