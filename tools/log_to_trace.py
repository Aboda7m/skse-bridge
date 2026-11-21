#!/usr/bin/env python3
"""Convert real SKSE-Bridge.log files (release builds, no trace) into trace events for tools/trace_replay.html.

    python tools/log_to_trace.py OUT_DIR LOG [LOG ...]       (or import convert() - see tools/bridge_viz.py)

Each LOG is replayed as a separate run, one after another (2 s apart). Only what the plain log contains is reconstructed:
lifecycle, table stats, patches, messaging, shadow VM, co-save, faults. There are NO `probe` events (those need a
--trace=y build), so no ID-lookup beams. Output is marked build="log-import".
"""
import json, os, re, sys
from datetime import datetime

LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\] \[t(\d+)\] (.*)$")


def convert(logs):
    """logs: list of SKSE-Bridge.log paths -> list of trace event dicts (sorted)."""
    events, seq, base_ms = [], 0, 0.0

    def ev(t, kind, plugin=None, **kw):
        nonlocal seq
        seq += 1
        e = {"t": round(base_ms + t, 3), "seq": seq, "ev": kind}
        if plugin:
            e["plugin"] = plugin
        e.update(kw)
        events.append(e)

    for path in logs:
        rows = []
        for raw in open(path, encoding="utf-8", errors="replace"):
            m = LINE.match(raw.rstrip("\n"))
            if m:
                rows.append((datetime.strptime(m[1], "%Y-%m-%d %H:%M:%S.%f"), int(m[2]), m[3]))
        if not rows:
            continue
        t0 = rows[0][0]
        cur = None                      # plugin currently being loaded
        nplug = 0
        seen = {}                       # (kind, plugin, key) -> running total
        info = {"game": "?", "table": {}, "ver": "?"}
        for ts, tid, msg in rows:
            t = (ts - t0).total_seconds() * 1000.0
            s = msg.strip()
            m = re.match(r"SKSE-Bridge ([\d.]+)", s)
            if m:
                info["ver"] = m[1]
                continue
            m = re.match(r"game exe: .* version ([\d.]+)", s)
            if m:
                info["game"] = m[1]
                ev(0, "run_start", run_id=os.path.basename(os.path.dirname(os.path.abspath(path))), build="log-import",
                   game_version=m[1], lookups=False, patches=True, table_dump=0)
                ev(1, "host", version=info["ver"], dry_run=False, translate=True)
                continue
            m = re.match(r"library: .* IDs=(\d+)", s) or re.match(r"preflight: game .* with (\d+) IDs", s)
            if m:
                info["rows"] = int(m[1])
            m = re.match(r"preflight: (\d+) of (\d+) legacy IDs exist .*; (\d+) do not", s)
            if m:
                info["rows"], info["missing"] = int(m[2]), int(m[3])
            m = re.match(r'skse proxy: hosted plugin "(.+?)" \((.+?)\) declared', s)
            if m:
                cur = m[2]
                nplug += 1
                ev(t, "plugin_begin", cur, index=nplug - 1, of=1, sha256="0" * 64, lineage="A")
                continue
            m = re.match(r"sha256: (\w+)", s)
            if m and cur:
                for e in reversed(events):
                    if e["ev"] == "plugin_begin" and e.get("plugin") == cur:
                        e["sha256"] = m[1]
                        break
                continue
            m = re.match(r"lineage: (.+?) \(", s)
            if m and cur:
                for e in reversed(events):
                    if e["ev"] == "plugin_begin" and e.get("plugin") == cur:
                        e["lineage"] = m[1]
                        break
                continue
            m = re.match(r"table: (\d+) rows, (\d+) translated, (\d+) missing.*?(\d+) remap", s)
            if m and cur:
                ev(t, "table", cur, rows=int(m[1]), translated=int(m[2]), remapped=int(m[4]), missing=int(m[3]), csv="", csv_mode=0)
                info["table"][cur] = True
                continue
            m = re.match(r"target: built for ([\d.]+), game ([\d.]+) -> (.*)", s)
            if m and cur:
                ev(t, "plan", cur, target=m[1], translate="native" not in m[3], why=m[3][:120])
                if "native" in m[3] and not info["table"].get(cur):
                    r = info.get("rows", 0)
                    ev(t, "table", cur, rows=r, translated=r, remapped=0, missing=0, csv="", csv_mode=0)
                continue
            m = re.match(r'\[interpose\] (.+?): OpenFileMapping[AW]\("(.+?)"\) -> (.+?) \((\d+) bytes\)', s)
            if m:
                ev(t, "table_open", m[1], mapping=m[2], bytes=int(m[4]))
                ev(t, "table_view", m[1], armed=False, bytes=int(m[4]), rows=info.get("rows", 0))
                continue
            m = re.match(r"\[vm-shadow\] EXPERIMENTAL shadow (\w+) ready: game layout ([\d.]+)", s)
            if m:
                ev(t, "shadow_ready", None, **{"class": m[1]}, ok=True, game_layout=m[2], real_global="0x0", bytes=0)
                continue
            m = re.match(r"\[vm-shadow\] (.+?) uses the shadow with the ([\d.]+) (\w+) layout", s)
            if m:
                ev(t, "shadow_attach", m[1], classes=m[3], layout=m[2])
                continue
            m = re.match(r"T0 LoadLibraryW -> \w+ \(GetLastError=(\d+)\)", s)
            if m and cur:
                ev(t, "dll_loaded", cur, ok=m[1] == "0", last_error=int(m[1]))
                continue
            if s.startswith("calling SKSEPlugin_Load") and cur:
                ev(t, "skse_load_begin", cur)
                continue
            m = re.match(r"T1 SKSEPlugin_Load returned (\w+)", s)
            if m and cur:
                ev(t, "skse_load_end", cur, returned=True, result=m[1] == "true", seh_code="0x0")
                ev(t + 0.5, "plugin_end", cur)
                continue
            if s == "done":
                ev(t, "load_phase_end")
                continue
            m = re.match(r"\[serialization\] (.+?): SetUniqueID\('(\w+)'\)", s)
            if m:
                ev(t, "cosave", m[1], phase="claim", uid="'%s'" % m[2])
                continue
            m = re.match(r"\[cosave\] bridge registered co-save '(\w+)'", s)
            if m:
                ev(t, "cosave", None, phase="register", uid="'%s'" % m[1])
                continue
            m = re.match(r'\[messaging\] (.+?) listens to "(.+?)" \((.+)\)', s)
            if m:
                ev(t, "msg_listen", m[1], sender=m[2], internal=m[2] != "SKSE", ok="REFUSED" not in m[3])
                continue
            m = re.match(r"\[messaging\] delivering message type (\d+) from (\S+) to (\S+)", s)
            if m:
                k = ("msg", m[3], m[1])
                seen[k] = seen.get(k, 0) + 1
                ev(t, "msg", m[3], **{"from": m[2]}, type=int(m[1]), len=0, route="skse" if m[2] == "SKSE" else "internal", n=seen[k], d=1)
                continue
            m = re.match(r"\[patch\] (.+?): (\d+) bytes at (.+?): .*?(?:\. (.*))?$", s)
            if m:
                note = m[4] or ""
                verdict = "blocked" if "block" in note.lower() else "warned" if "warn" in note.lower() else "ok"
                ev(t, "patch", m[1], verdict=verdict, size=int(m[2]), where=m[3], note=note[:80])
                continue
            m = re.match(r"\[vm-shadow\] (.+?) reads (\w+)\+(0x[0-9A-Fa-f]+) \(([\d.]+) layout\) -> real \+(0x[0-9A-Fa-f]+)", s)
            if m:
                k = ("vm", m[1], m[2], m[3])
                seen[k] = max(seen.get(k, 0), 1)
                ev(t, "vm_access", m[1], **{"class": m[2]}, game=False, whole=False, off=m[3], real=m[5], write=False,
                   layout=m[4], target="real", n=1, d=1)
                continue
            m = re.match(r"\[vm-shadow\]\s+(.+?) (\w+)\+(0x[0-9A-Fa-f]+) -> \+(0x[0-9A-Fa-f]+) (read|write) x(\d+)", s)
            if m:
                k = ("vm", m[1], m[2], m[3])
                n = int(m[6])
                d = n - seen.get(k, 0)
                if d > 0:
                    seen[k] = n
                    ev(t, "vm_access", m[1], **{"class": m[2]}, game=False, whole=False, off=m[3], real=m[4], write=m[5] == "write",
                       layout="1.7", target="real", n=n, d=d)
                continue
            if "poisoned address" in s or s.startswith("fault:"):
                ev(t, "fault", cur, code="0xC0000005", ip="0x0", inside_plugin=True, from_plugin=False, missing_id=True, detail=s[:160])
        end_t = (rows[-1][0] - t0).total_seconds() * 1000.0
        ev(end_t + 100, "heartbeat", probes=0, probes_dropped=0)
        base_ms += end_t + 2000.0

    events.sort(key=lambda e: (e["t"], e["seq"]))
    return events


if __name__ == "__main__":
    out_dir, logs = sys.argv[1], sys.argv[2:]
    os.makedirs(out_dir, exist_ok=True)
    evs = convert(logs)
    with open(os.path.join(out_dir, "SKSE-Bridge-trace.jsonl"), "w", encoding="utf-8") as f:
        for e in evs:
            f.write(json.dumps(e, separators=(",", ":")) + "\n")
    print("wrote", out_dir, "-", len(evs), "events")
