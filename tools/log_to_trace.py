#!/usr/bin/env python3
"""Convert real SKSE-Bridge.log files (release builds, no trace) into trace events for tools/trace_replay.html.

    python tools/log_to_trace.py OUT_DIR LOG [LOG ...]       (or import convert() - see tools/bridge_viz.py)

Each LOG is replayed as a separate run, one after another (2 s apart). Only what the plain log contains is reconstructed:
lifecycle, Address Library / table stats, patches, messaging, shadow VM (SkyrimVM + PlayerCharacter), co-save, faults.
There are NO `probe` events (those need a --trace=y build), so no ID-lookup beams. Output is marked build="log-import".

The patterns below follow the format strings in src/*.cpp (src/vmshadow.cpp: "[shadow] ..."; src/main.cpp: table, target,
shadow decisions; src/skseproxy.cpp: messaging / cosave). Builds before 0.7 wrote "[vm-shadow]"; both tags are accepted.

Pass stats={} to convert() to learn what it could NOT parse: stats["unmatched"] maps a log tag ("[shadow]", "[patch]", ...)
to the lines that carry that tag but matched no pattern. A non-empty list for a tag the converter claims to handle means the
log format and this file have drifted apart again (that is how the missing shadow events went unnoticed).
"""
import json, os, re, sys
from datetime import datetime

LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\] \[t(\d+)\] (.*)$")
TAG = r"\[(?:vm-)?shadow\]"          # 0.7.x writes [shadow]; older builds wrote [vm-shadow]
HEX = r"0x[0-9A-Fa-f]+"
GAME_CODE = "<game code"             # "<game code>" and "<game code, whole object>" (src/vmshadow.cpp)

# Member layout of each shadowed class, from kClasses[] in src/vmshadow.cpp: every member at or after insert_at moved by
# `inserted` bytes in the 1.7.99+ layout. The replay uses it to explain a shadow offset -> real offset mapping.
SHADOW_CLASSES = {
    "SkyrimVM": {"id": 400475, "insert_at": "0x180", "inserted": 0x10, "span": 0x9000},
    "PlayerCharacter": {"id": 403521, "insert_at": "0x2D8", "inserted": 0x8, "span": 0x2000},
}
TARGETS = {"real": "real", "dummy (object not created yet)": "pending", "dummy (member does not exist in this runtime)": "absent"}
KNOWN_TAGS = ("[shadow]", "[vm-shadow]", "[patch]", "[cosave]", "[serialization]", "[messaging]", "[interpose]", "[fault]")


def _unmatched(stats, s):
    if stats is None:
        return
    m = re.match(r"^(\[[\w-]+\])", s)
    if m and m[1] in KNOWN_TAGS:
        stats.setdefault("unmatched", {}).setdefault(m[1], []).append(s[:200])


def convert(logs, stats=None):
    """logs: list of SKSE-Bridge.log paths -> list of trace event dicts (sorted)."""
    events, seq, base_ms = [], 0, 0.0
    if stats is not None:
        stats.setdefault("matched", 0)
        stats.setdefault("unmatched", {})

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
            m = LINE.match(raw.rstrip("\r\n"))
            if m:
                rows.append((datetime.strptime(m[1], "%Y-%m-%d %H:%M:%S.%f"), int(m[2]), m[3]))
        if not rows:
            continue
        t0 = rows[0][0]
        cur = None                      # plugin currently being loaded
        nplug = 0
        seen = {}                       # running totals: ("msg", plugin, type) / ("vm", class, who, shadow offset)
        vm = {"target": {}, "layout": {}, "write": {}, "attach": {}}   # per access key / per plugin facts learnt so far
        info = {"game": "?", "table": {}, "ver": "?", "game_layout": "1.7"}

        def set_plugin_field(name, field, value):
            for e in reversed(events):
                if e["ev"] == "plugin_begin" and e.get("plugin") == name:
                    e[field] = value
                    return

        def vm_event(t, cls, who, off, real, write, layout, target, n, d):
            game = who.startswith(GAME_CODE)
            ev(t, "vm_access", None if game else who, **{"class": cls}, game=game, whole="whole object" in who, off=off, real=real,
               write=write, layout=layout, target=target, n=n, d=d)

        for ts, tid, msg in rows:
            t = (ts - t0).total_seconds() * 1000.0
            s = msg.strip()
            n_before = len(events)
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

            # ---- Address Library / table -------------------------------------------------------------------------
            m = re.match(r"library: (.*) format=(\d+) IDs=(\d+)", s)
            if m:
                info["rows"] = int(m[3])
                ev(t, "idlib", None, file=os.path.basename(m[1].replace("\\", "/")), format=int(m[2]), rows=int(m[3]))
                continue
            m = re.match(r"preflight: game ([\d.]+), Address Library format (\d+) with (\d+) IDs", s)
            if m:
                info["rows"] = int(m[3])
                ev(t, "preflight", None, ok=True, game=m[1], format=int(m[2]), rows=int(m[3]))
                continue
            m = re.match(r"preflight: (\d+) rule\(s\) from (.+)", s)
            if m:
                ev(t, "preflight_rules", None, rules=int(m[1]), file=os.path.basename(m[2].replace("\\", "/")))
                continue
            m = re.match(r"preflight: (ok|FAILED: ?(.*))$", s)
            if m:
                if m[1] != "ok":
                    ev(t, "preflight", None, ok=False, why=(m[2] or "")[:160])
                continue
            m = re.match(r'skse proxy: hosted plugin "(.+?)" \((.+?)\) declared', s)
            if m:
                cur = m[2]
                nplug += 1
                ev(t, "plugin_begin", cur, index=nplug - 1, of=1, sha256="0" * 64, lineage="A")
                continue
            m = re.match(r"sha256: (\w+)", s)
            if m and cur:
                set_plugin_field(cur, "sha256", m[1])
                continue
            m = re.match(r"lineage: (.+?) \(", s)
            if m and cur:
                set_plugin_field(cur, "lineage", m[1])
                continue
            m = re.match(r"remaps: (\d+) known, (\d+) inferred .*?, (\d+) global, (\d+) from (.+)$", s)
            if m and cur:
                ev(t, "table_remaps", cur, known=int(m[1]), inferred=int(m[2]), glob=int(m[3]), overlay=int(m[4]),
                   overlay_file="" if m[5] == "no overlay" else os.path.basename(m[5].replace("\\", "/")))
                continue
            m = re.match(r"table: (\d+) rows, (\d+) translated, (\d+) missing.*?(\d+) remap", s)
            if m and cur:
                ev(t, "table", cur, rows=int(m[1]), translated=int(m[2]), remapped=int(m[4]), missing=int(m[3]), csv="", csv_mode=0)
                info["table"][cur] = True
                continue
            m = re.match(r"target: built for ([\d.]+), game ([\d.]+) -> (.*)", s)
            if m and cur:
                native = "native" in m[3] and "TRANSLATED" not in m[3]
                ev(t, "plan", cur, target=m[1], translate=not native, why=m[3][:120])
                if native and not info["table"].get(cur):
                    # a natively loaded plugin reads the game's own table: every ID the running game has, none translated
                    r = info.get("rows", 0)
                    ev(t, "table", cur, rows=r, translated=r, remapped=0, missing=0, csv="", csv_mode=0, native=True, built_for=m[1])
                continue
            m = re.match(r"shadow: ID (\d+) \((\w+)\) in this plugin's table -> shadow global", s)
            if m and cur:
                ev(t, "shadow_table", cur, id=int(m[1]), **{"class": m[2]})
                continue
            m = re.match(r"(vm|player)-shadow: (ON|off)", s)
            if m and cur:
                ev(t, "shadow_choice", cur, **{"class": "SkyrimVM" if m[1] == "vm" else "PlayerCharacter"}, on=m[2] == "ON")
                continue
            m = re.match(r'\[interpose\] (.+?): OpenFileMapping[AW]\("(.+?)"\) -> (private|bridge) table \((\d+) bytes\)', s)
            if m:
                ev(t, "table_open", m[1], mapping=m[2], bytes=int(m[4]), table_kind=m[3])
                ev(t, "table_view", m[1], armed=False, bytes=int(m[4]), rows=info.get("rows", 0), why="plain log (no --trace=y build)")
                continue
            m = re.match(r'\[interpose\] (.+?): (?:OpenFileMapping[AW]|CreateFileMapping[AW])\("(.+?)"\)(?::)? (REFUSED|no private table; refusing|DuplicateHandle failed)', s)
            if m:
                ev(t, "table_refused", m[1], mapping=m[2], why=m[3])
                continue

            # ---- shadow VM (src/vmshadow.cpp) --------------------------------------------------------------------
            m = re.match(TAG + r" EXPERIMENTAL shadow (\w+) ready: game layout ([\d.]+)(?:, global (\S+) \(real global SkyrimSE\.exe\+(" + HEX
                         + r")\), shadow (\S+) \(\+(" + HEX + r")\))?", s)
            if m:
                info["game_layout"] = m[2]
                kc = SHADOW_CLASSES.get(m[1], {})
                ev(t, "shadow_ready", None, **{"class": m[1]}, ok=True, game_layout=m[2], real_global=m[4] or "0x0",
                   bytes=int(m[6], 16) if m[6] else kc.get("span", 0), insert_at=kc.get("insert_at", ""), inserted=kc.get("inserted", 0),
                   id=kc.get("id", 0))
                continue
            m = re.match(TAG + r" (\w+): ID (\d+) not in this game's Address Library; shadow unavailable", s)
            if m:
                ev(t, "shadow_ready", None, **{"class": m[1]}, ok=False, why="ID %s not in this game's Address Library" % m[2])
                continue
            m = re.match(TAG + r" (\w+): could not reserve memory near the game image; shadow unavailable", s)
            if m:
                ev(t, "shadow_ready", None, **{"class": m[1]}, ok=False, why="no memory near the game image")
                continue
            m = re.match(TAG + r" (.+?) uses the shadow(?:\(s\))?((?: \w+)+) with the ([\d.]+) layout", s)
            if m:
                classes = m[2].split()
                vm["attach"][m[1]] = m[3]
                ev(t, "shadow_attach", m[1], classes=",".join(classes), layout=m[3])
                continue
            m = re.match(TAG + r" (.+?) uses the shadow with the ([\d.]+) (\w+) layout", s)      # pre-0.7 wording
            if m:
                vm["attach"][m[1]] = m[2]
                ev(t, "shadow_attach", m[1], classes=m[3], layout=m[2])
                continue
            m = re.match(TAG + r" summary \((.+?)\): (\d+) member access\(es\), (\d+) global read\(s\), (\d+) from game code, (\d+) unresolved", s)
            if m:
                ev(t, "vm_summary", None, when=m[1], accesses=int(m[2]), global_reads=int(m[3]), game_side=int(m[4]), unresolved=int(m[5]))
                continue
            # summary rows: "<Class> <plugin|<game code>> +0xSHADOW -> +0xREAL read|written xN (first at RIP)"
            m = re.match(TAG + r"\s+(\w+) (.+?) \+(" + HEX + r") -> \+(" + HEX + r") (read|written) x(\d+)(?: \(first at (\S+)\))?$", s)
            if m:
                cls, who, off, real, rw, n = m[1], m[2], m[3], m[4], m[5], int(m[6])
                k = ("vm", cls, who, off)
                d = n - seen.get(k, 0)
                if d > 0:
                    seen[k] = n
                    write = rw == "written"
                    vm_event(t, cls, who, off, real, write, vm["layout"].get(k) or vm["attach"].get(who) or info["game_layout"],
                             vm["target"].get(k, "real"), n, d)
                continue
            # first access of a member: "<plugin|<game code>> reads|writes <Class>+0xSHADOW (1.7 layout) -> real +0xREAL | dummy (...) at RIP p"
            m = re.match(TAG + r" (.+?) (reads|writes) (\w+)\+(" + HEX + r") \((1\.[67]) layout\) -> (real \+(" + HEX + r")|dummy \([^)]*\))(?: at RIP (\S+))?$", s)
            if m:
                who, rw, cls, off, layout, dest = m[1], m[2], m[3], m[4], m[5], m[6]
                target = "real" if dest.startswith("real") else TARGETS.get(dest, "absent")
                k = ("vm", cls, who, off)
                vm["target"][k], vm["layout"][k] = target, layout
                if k not in seen:
                    seen[k] = 1
                    vm_event(t, cls, who, off, m[7] or "0x0", rw == "writes", layout, target, 1, 1)
                continue
            m = re.match(TAG + r" unresolved (read|write) of the (\w+) global at RIP (\S+)", s)
            if m:
                ev(t, "fault", None, code="shadow-global", ip=m[3], inside_plugin=False, from_plugin=False, missing_id=False,
                   detail="shadow: unresolved %s of the %s global at RIP %s" % (m[1], m[2], m[3]))
                continue
            m = re.match(TAG + r" UNRESOLVED instruction on (\w+) at RIP (\S+) \(bytes (.*?)\)", s)
            if m:
                ev(t, "fault", None, code="shadow-insn", ip=m[2], inside_plugin=False, from_plugin=False, missing_id=False,
                   detail="shadow: UNRESOLVED instruction on %s at RIP %s (bytes %s)" % (m[1], m[2], m[3]))
                continue

            # ---- lifecycle / messaging / co-save / patches -----------------------------------------------------------
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
            m = re.match(r"\[cosave\] bridge registered co-save '?(\w+)'?", s)
            if m:
                ev(t, "cosave", None, phase="register", uid="'%s'" % m[1])
                continue
            m = re.match(r'\[messaging\] (.+?) listens to (?:"(.+?)"|all senders)(.*)', s)
            if m:
                sender = m[2] or "*"
                ev(t, "msg_listen", m[1], sender=sender, internal=sender not in ("SKSE", "*"), ok="REFUSED" not in m[3])
                continue
            m = re.match(r"\[messaging\] delivering message type (\d+) from (\S+) to (\S+)", s)
            if m:
                k = ("msg", m[3], m[1])
                seen[k] = seen.get(k, 0) + 1
                ev(t, "msg", m[3], **{"from": m[2]}, type=int(m[1]), len=0, route="skse" if m[2] == "SKSE" else "internal", n=seen[k], d=1)
                continue
            m = re.match(r"\[patch\] (.+?): BLOCKED (\d+)-byte write at (.+?): (.*?)\. Refused", s)
            if m:
                ev(t, "patch", m[1], verdict="blocked", size=int(m[2]), where=m[3], note=m[4][:80])
                continue
            m = re.match(r"\[patch\] (.+?): (\d+) bytes at (.+?) made writable and not restored \(yet\): .*? -> .*?\. (.*)$", s)
            if m:
                ev(t, "patch", m[1], verdict="warned", size=int(m[2]), where=m[3], note=("left writable: " + m[4])[:80])
                continue
            m = re.match(r"\[patch\] (.+?): (\d+) bytes at (.+?): (.*?) -> (.*?)\. (.*)$", s)
            if m:
                note = m[6]
                verdict = "unsafe" if note.startswith("UNSAFE") else "warned" if note.startswith("WARNING") else "ok"
                ev(t, "patch", m[1], verdict=verdict, size=int(m[2]), where=m[3], note=note[:80])
                continue
            m = re.match(r"\[patch\] summary (.+?) \((.+?)\): (\d+) ok, (\d+) warned, (\d+) blocked, (\d+) left writable", s)
            if m:
                ev(t, "patch_summary", m[1], when=m[2], ok=int(m[3]), warned=int(m[4]), blocked=int(m[5]), left_writable=int(m[6]))
                continue
            if s.startswith("[patch] log limit reached"):
                continue
            if "poisoned address" in s or s.startswith("[fault]") or s.startswith("fault:"):
                ev(t, "fault", cur, code="0xC0000005", ip="0x0", inside_plugin=True, from_plugin=False, missing_id=True, detail=s[:160])
                continue
            if len(events) == n_before:
                _unmatched(stats, s)
            elif stats is not None:
                stats["matched"] += 1
        end_t = (rows[-1][0] - t0).total_seconds() * 1000.0
        ev(end_t + 100, "heartbeat", probes=0, probes_dropped=0)
        base_ms += end_t + 2000.0

    events.sort(key=lambda e: (e["t"], e["seq"]))
    return events


if __name__ == "__main__":
    out_dir, logs = sys.argv[1], sys.argv[2:]
    os.makedirs(out_dir, exist_ok=True)
    st = {}
    evs = convert(logs, st)
    with open(os.path.join(out_dir, "SKSE-Bridge-trace.jsonl"), "w", encoding="utf-8") as f:
        for e in evs:
            f.write(json.dumps(e, separators=(",", ":")) + "\n")
    print("wrote", out_dir, "-", len(evs), "events")
    for tag, lines in st.get("unmatched", {}).items():
        print("  NOT PARSED %s: %d line(s), e.g. %s" % (tag, len(lines), lines[0]))
