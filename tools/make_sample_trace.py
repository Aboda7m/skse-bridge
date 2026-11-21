#!/usr/bin/env python3
"""Generate a synthetic SKSE-Bridge-trace.jsonl (+ table CSV) so trace_viz.py / trace_replay.html can be tried without the game.

    python tools/make_sample_trace.py [out_dir]        # default: ./trace-out/sample

The probe pattern mimics what the bridge records from a real plugin: a std::lower_bound over the sorted table, so the first
probe of every lookup is entry N//2 and the last one is the entry that was found.
"""
import json
import os
import random
import sys

out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join("trace-out", "sample")
os.makedirs(out_dir, exist_ok=True)
random.seed(7)

N = 428461
ids = [int(i * 1.17) + 3 for i in range(N)]                 # sorted legacy IDs, one per table row
missing_ids = set(random.sample(ids, 35765))                 # ~8% do not exist in the new game library
remapped_ids = set(random.sample(ids, 60))

plugins = [
    ("RTL-SUPPORT-NG.dll",       900, 0.00),
    ("PickPocketRedone-NG.dll", 1300, 1.10),
    ("ImprovedCameraSE.dll",    2400, 2.60),
    ("CompassNavigationOverhaul.dll", 1700, 4.80),
    ("InfinityUI.dll",         1100, 6.60),
]
SHADOW_VM = {"PickPocketRedone-NG.dll": "SkyrimVM", "ImprovedCameraSE.dll": "PlayerCharacter", "CompassNavigationOverhaul.dll": "SkyrimVM,PlayerCharacter"}

events = []
seq = 0
def ev(t, kind, plugin=None, **kw):
    global seq
    seq += 1
    e = {"t": round(t, 3), "seq": seq, "ev": kind}
    if plugin:
        e["plugin"] = plugin
    e.update(kw)
    events.append(e)

ev(0, "run_start", run_id="sample", build="trace", game_version="1.7.104.0", lookups=True, patches=True, table_dump=1)
ev(1, "host", version="0.7.3.0", dry_run=False, translate=True)

rows = []
for name, nlook, t0 in plugins:
    t = t0 * 1000
    ev(t, "plugin_begin", name, index=0, of=len(plugins), sha256="0" * 64, lineage="A")
    ev(t + 5, "plan", name, target="1.6.1170.0", translate=True, why="")
    miss = sum(1 for _ in missing_ids)
    ev(t + 120, "table", name, rows=N, translated=N - len(missing_ids) - len(remapped_ids), remapped=len(remapped_ids),
       missing=len(missing_ids), csv="SKSE-Bridge-trace-table-%s.csv" % name, csv_mode=1)
    ev(t + 130, "table_open", name, mapping="CommonLibSSEOffsets-v2-1.6.1170.0", bytes=N * 16)
    ev(t + 131, "table_view", name, armed=True, bytes=N * 16, rows=N)
    if name in SHADOW_VM:
        ev(t + 150, "shadow_attach", name, classes=SHADOW_VM[name], layout="1.6")
    ev(t + 160, "msg_listen", name, sender="SKSE", internal=False, ok=True)
    if name.startswith("Compass"):
        ev(t + 161, "msg_listen", name, sender="InfinityUI", internal=True, ok=True)
    if name.startswith("Infinity"):
        ev(t + 161, "msg_listen", name, sender="CompassNavigationOverhaul", internal=True, ok=True)
    if name.startswith(("Compass", "Pick")):
        ev(t + 165, "cosave", name, phase="claim", uid="'%s'" % ("CNOV" if name.startswith("Compass") else "PPKT"))
    ev(t + 180, "dll_loaded", name, ok=True, last_error=0)
    ev(t + 190, "skse_load_begin", name)
    tt = t + 200
    for k in range(nlook):
        target_idx = min(N - 1, int(abs(random.gauss(0, 0.33)) * N)) if random.random() < 0.7 else random.randrange(N)
        target = ids[target_idx]
        lo, count = 0, N
        while count > 0:
            step = count // 2
            mid = lo + step
            tt += random.uniform(0.002, 0.012)
            ev(tt, "probe", name, tid=4242, entry=mid, id=ids[mid])
            if ids[mid] < target:
                lo = mid + 1
                count -= step + 1
            else:
                count = step
        tt += 0.004
        ev(tt, "probe", name, tid=4242, entry=lo, id=ids[lo])
        tt += random.uniform(0.05, 0.9)
        if target in missing_ids:
            rows.append((target, "missing"))
        if k % 450 == 449 and name.startswith(("Improved", "Pick", "Compass")):
            ev(tt, "patch", name, verdict=random.choice(["ok", "warned", "blocked"]), size=6, where="ID %d+0x%X" % (target, random.randrange(16, 400)), note="sample")
    if name.startswith("Compass"):
        ev(tt, "fault", name, code="0xC0000005", ip="0x7FF612340000", inside_plugin=True, from_plugin=False, missing_id=True,
           detail="rax holds a poisoned address: missing Address Library ID %d (+ displacement, if any)" % next(iter(missing_ids)))
    ev(tt + 3, "skse_load_end", name, returned=True, result=True, seh_code="0x0")
    ev(tt + 4, "plugin_end", name)
ev(tt + 10, "load_phase_end")

# ---- in-game phase: shadow VM redirects, messaging between hosted plugins, co-save ----
G0 = tt + 400
ev(G0 - 300, "shadow_ready", None, **{"class": "SkyrimVM"}, ok=True, game_layout="1.7", real_global="0x2FC76C8", bytes=36864)
ev(G0 - 299, "shadow_ready", None, **{"class": "PlayerCharacter"}, ok=True, game_layout="1.7", real_global="0x2F26EF8", bytes=4096)
ev(G0 - 100, "cosave", None, phase="register", uid="'SKBR'")
names = [p[0] for p in plugins]
for i, n in enumerate(names):                                   # SKSE kDataLoaded (type 8) fanned out to every listener
    ev(G0 + i * 3, "msg", n, **{"from": "SKSE"}, type=8, len=0, route="skse", n=1, d=1)
ev(G0 + 45, "cosave", "CompassNavigationOverhaul.dll", phase="load", uid="'CNOV'", records=3, read=3)
ev(G0 + 47, "cosave", "PickPocketRedone-NG.dll", phase="load", uid="'PPKT'", records=1, read=1)
ev(G0 + 49, "cosave", None, phase="load_orphans", records=2)

KEYS = [  # (plugin or None for game code, class, off, real, write, target, per-100ms rate)
    ("PickPocketRedone-NG.dll", "SkyrimVM", "0x200", "0x210", False, "real", 14),
    ("PickPocketRedone-NG.dll", "SkyrimVM", "0x2A8", "0x2B8", True, "real", 2),
    ("ImprovedCameraSE.dll", "PlayerCharacter", "0x6D0", "0x6D0", False, "real", 40),
    ("ImprovedCameraSE.dll", "PlayerCharacter", "0x6F0", "0x6F0", True, "real", 9),
    ("CompassNavigationOverhaul.dll", "PlayerCharacter", "0x8C0", "0x8D0", False, "real", 22),
    ("CompassNavigationOverhaul.dll", "SkyrimVM", "0x210", "0x220", False, "absent", 1),
    (None, "SkyrimVM", "0x0", "0x0", False, "real", 5),
]
tot = {k: 0 for k in range(len(KEYS))}
tm = G0 + 60
for step in range(110):                                          # ~11 s of play, one flush per 100 ms
    tm += 100
    for ki, (pl, cls, off, real, wr, tgt, rate) in enumerate(KEYS):
        if ki == 5 and step % 15:                                # rare
            continue
        d = max(1, int(random.gauss(rate, rate * 0.3)))
        if step < 4 and tgt == "real" and ki < 2:               # VM not created yet during the first 400 ms
            tgt_now = "pending"
        else:
            tgt_now = tgt
        tot[ki] += d
        kw = {"class": cls}
        e = dict(game=pl is None, whole=pl is None, off=off, real=real, write=wr, layout="1.6", target=tgt_now, n=tot[ki], d=d)
        ev(tm + ki, "vm_access", pl, **kw, **e)
    if step % 9 == 4:                                            # hosted plugins talking to each other
        ev(tm + 20, "msg", "InfinityUI.dll", **{"from": "CompassNavigationOverhaul"}, type=1 + step % 3, len=16, route="internal", n=step, d=3)
    if step % 9 == 8:
        ev(tm + 30, "msg", "CompassNavigationOverhaul.dll", **{"from": "InfinityUI"}, type=7, len=8, route="internal", n=step, d=2)
    if step % 31 == 5:
        ev(tm + 40, "msg_out", "CompassNavigationOverhaul.dll", **{"from": "CompassNavigationOverhaul"}, to="", type=9, len=4, delivered=True, n=step, d=1)
    if step == 40:                                               # player saves
        for n, u, r in (("CompassNavigationOverhaul.dll", "CNOV", 3), ("PickPocketRedone-NG.dll", "PPKT", 1)):
            ev(tm + 60, "cosave", n, phase="save", uid="'%s'" % u, records=r)
        ev(tm + 65, "cosave", None, phase="save_orphans", records=2)
ev(tm + 500, "heartbeat", probes=sum(1 for e in events if e["ev"] == "probe"), probes_dropped=0)

with open(os.path.join(out_dir, "SKSE-Bridge-trace.jsonl"), "w", encoding="utf-8") as f:
    for e in sorted(events, key=lambda e: e["t"]):
        f.write(json.dumps(e, separators=(",", ":")) + "\n")

for name, _, _ in plugins:
    with open(os.path.join(out_dir, "SKSE-Bridge-trace-table-%s.csv" % name), "w", encoding="utf-8") as f:
        f.write("id,legacy_offset,new_offset,status\n")
        for i in sorted(missing_ids):
            f.write("%d,%d,,missing\n" % (i, i * 16))
        for i in sorted(remapped_ids - missing_ids):
            f.write("%d,%d,%d,remapped\n" % (i, i * 16, i * 16 + 64))

print("wrote", out_dir, "-", len(events), "events")
