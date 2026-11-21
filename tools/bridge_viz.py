#!/usr/bin/env python3
"""SKSE-Bridge log -> visualisation, one command. Works on any user's logs.

    python bridge_viz.py [INPUT ...] [--out DIR] [--no-video] [--size 1920x1080] [--speed 1] [--combine]

INPUT can be any mix of (default: your SKSE log folder in Documents\\My Games\\Skyrim Special Edition*\\SKSE):
  * a SKSE folder         - the current SKSE-Bridge.log (+ trace .jsonl if present) AND everything in SKSE-Bridge-history\\
  * a SKSE-Bridge*.log    - one session (plain release log)
  * a *-trace.jsonl       - one session from a --trace=y build (exact lookups); used instead of the log it belongs to
  * a .zip a tester sent  - unpacked into a temp folder, then handled like a folder
  * a folder of any of the above (searched recursively)

Per session it writes, into DIR\\<session-name>\\ (default: .\\bridge-viz-out):
  replay.html   self-contained interactive replay (double-click, no server, no internet)
  replay.webm   video of it (needs Edge / Chrome / Chromium installed; skipped with a note if none)
  report.txt    the setup (game, SKSE, plugins, config) and every problem the log shows
  trace.jsonl   the events the replay was built from
--combine also writes one replay/video that plays all sessions after each other.

A plain log has no per-ID lookups, so the Address Library node stays empty; trace builds (--trace=y) add them.
"""
import argparse, glob, json, os, re, shutil, sys, tempfile, zipfile

HERE = os.path.dirname(os.path.abspath(getattr(sys, "_MEIPASS", __file__)))
sys.path.insert(0, HERE)
import log_to_trace  # noqa: E402

LOG_RE = re.compile(r"^SKSE-Bridge(-\d{8}-\d{6})?\.log$", re.I)


# ---------------------------------------------------------------- finding sessions
def default_skse_dirs():
    docs = os.path.join(os.path.expanduser("~"), "Documents", "My Games")
    return [d for d in glob.glob(os.path.join(docs, "Skyrim Special Edition*", "SKSE")) if os.path.isdir(d)]


def safe_extract(zpath, dest):
    root = os.path.realpath(dest)
    with zipfile.ZipFile(zpath) as z:
        for m in z.infolist():
            target = os.path.realpath(os.path.join(dest, m.filename))
            if not (target == root or target.startswith(root + os.sep)):
                continue                                  # zip-slip: skip anything escaping the folder
            if m.file_size > 600 * 1024 * 1024:
                continue
            z.extract(m, dest)


def collect(inputs, tmp):
    """-> list of sessions {name, log, trace}. A trace pairs with the log of the same stamp (or the only current log)."""
    files = []
    for p in inputs:
        if os.path.isfile(p) and p.lower().endswith(".zip"):
            d = tempfile.mkdtemp(prefix="bv-", dir=tmp)
            safe_extract(p, d)
            p = d
        if os.path.isdir(p):
            for r, _, fs in os.walk(p):
                files += [os.path.join(r, f) for f in fs]
        elif os.path.isfile(p):
            files.append(p)
    sessions = {}
    for f in files:
        b = os.path.basename(f)
        parent = os.path.basename(os.path.dirname(f)) or "run"
        if LOG_RE.match(b) or (b.lower().endswith(".log") and "bridge" in b.lower()):
            key = (os.path.dirname(f), re.sub(r"^SKSE-Bridge", "", b[:-4]))
            sessions.setdefault(key, {})["log"] = f
        elif b.lower().endswith("-trace.jsonl") or re.match(r"^SKSE-Bridge(-\d{8}-\d{6})?-trace\.jsonl$", b, re.I):
            m = re.match(r"^SKSE-Bridge(-\d{8}-\d{6})?-trace", b, re.I)
            key = (os.path.dirname(f), m.group(1) or "" if m else "")
            sessions.setdefault(key, {})["trace"] = f
    out = []
    for (d, stamp), v in sessions.items():
        if not v:
            continue
        src = v.get("log") or v.get("trace")
        out.append({"dir": d, "stamp": stamp, "log": v.get("log"), "trace": v.get("trace"), "mtime": os.path.getmtime(src)})
    out.sort(key=lambda s: s["mtime"])
    # unique readable names: <folder>-<stamp or current>
    used = set()
    for s in out:
        base = re.sub(r"[^\w.-]+", "_", (os.path.basename(s["dir"]) if os.path.basename(s["dir"]).lower() not in ("skse", "skse-bridge-history") else os.path.basename(os.path.dirname(s["dir"])) + "_" + os.path.basename(s["dir"])) + (s["stamp"] or "_current")).strip("_") or "session"
        n, name = 1, base
        while name in used:
            n += 1
            name = "%s_%d" % (base, n)
        used.add(name)
        s["name"] = name
    return out


# ---------------------------------------------------------------- events + report
def load_events(s):
    if s["trace"]:
        ev = []
        for line in open(s["trace"], encoding="utf-8", errors="replace"):
            try:
                ev.append(json.loads(line))
            except ValueError:
                pass
        if ev:
            return ev, "trace (exact lookups)"
    if s["log"]:
        return log_to_trace.convert([s["log"]]), "plain log (no per-ID lookups)"
    return [], "nothing"


def report(s, events, source):
    L = []
    add = L.append
    add("SKSE-Bridge session report  -  %s" % s["name"])
    add("source: %s" % source)
    add("files : %s" % ", ".join(x for x in (s["log"], s["trace"]) if x))
    add("")
    if s["log"]:
        txt = open(s["log"], encoding="utf-8", errors="replace").read().splitlines()
        keep = ("SKSE-Bridge ", "game exe:", "SKSE interface:", "config:", "legacy plugins:", "preflight:", "library:", "patch audit:")
        add("== setup ==")
        for ln in txt:
            m = re.match(r"^\[[^\]]+\] \[t\d+\] (.*)$", ln)
            if m and m[1].startswith(keep):
                add("  " + m[1][:200])
        add("")
        add("== plugins ==")
        for ln in txt:
            m = re.match(r"^\[[^\]]+\] \[t\d+\]\s+(target: .*|lineage: .*|table: .*|SKSEPlugin_Version: .*|RTTI: .*)$", ln)
            h = re.match(r"^\[[^\]]+\] \[t\d+\] ---- (.*)$", ln)
            if h:
                add("  " + os.path.basename(h[1].replace("\\", "/")))
            elif m:
                add("      " + m[1][:200])
        add("")
        add("== problems / things to look at ==")
        bad = re.compile(r"ERROR|REFUSED|\b[1-9]\d* blocked|\b[1-9]\d* warned|NOT armed|they will fault|GetLastError=[1-9]|returned false|"
                         r"\b[1-9]\d* class\(es\) with a vtable change|[Ff]ailed|unresolved [1-9]|[Cc]rash|cannot create|\bfault:|poisoned address")
        noise = re.compile(r"^\S+ \(\d+\):|flagged imports|import ")
        n = 0
        for ln in txt:
            m = re.match(r"^\[([^\]]+)\] \[t\d+\] (.*)$", ln)
            if m and bad.search(m[2]) and not noise.search(m[2].strip()) and "log-only" not in m[2] and "fault log" not in m[2]:
                add("  %s  %s" % (m[1][11:], m[2].strip()[:220]))
                n += 1
        if not n:
            add("  none found in the log")
    kinds = {}
    for e in events:
        kinds[e["ev"]] = kinds.get(e["ev"], 0) + 1
    add("")
    add("== events in the replay ==")
    add("  " + ", ".join("%s=%d" % kv for kv in sorted(kinds.items())))
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------- html + video
def read_template():
    p = os.path.join(HERE, "trace_replay.html")
    if not os.path.isfile(p):
        p = os.path.join(HERE, "tools", "trace_replay.html")
    return open(p, encoding="utf-8").read()


def write_html(path, events):
    jl = "\n".join(json.dumps(e, separators=(",", ":")) for e in events)
    blob = "<script>window.__EMBEDDED_TRACE=%s;</script>\n" % json.dumps(jl).replace("</", "<\\/")
    html = read_template()
    i = html.index("<script>")
    open(path, "w", encoding="utf-8").write(html[:i] + blob + html[i:])


def record(html_path, out_webm, size, speed):
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        return "video skipped: install it with  pip install playwright  (it then uses your installed Edge/Chrome)"
    with sync_playwright() as p:
        b = None
        for kw in ({"channel": "msedge"}, {"channel": "chrome"}, {}):
            try:
                b = p.chromium.launch(args=["--autoplay-policy=no-user-gesture-required"], **kw)
                break
            except Exception:
                continue
        if not b:
            return "video skipped: no Edge/Chrome/Chromium found (the replay.html still works)"
        try:
            pg = b.new_page(accept_downloads=True, viewport={"width": 1500, "height": 900})
            pg.goto("file:///" + os.path.abspath(html_path).replace("\\", "/").lstrip("/"))
            pg.wait_for_selector("#rec:not([disabled])", timeout=20000)
            pg.select_option("#recsize", size)
            pg.evaluate("s => { const e = document.getElementById('speed'); e.value = s; e.dispatchEvent(new Event('input')); }", str(speed))
            with pg.expect_download(timeout=900000) as d:
                pg.click("#rec")
            d.value.save_as(out_webm)
        finally:
            b.close()
    return "video: " + out_webm


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description="SKSE-Bridge logs -> replay.html + video + report")
    ap.add_argument("inputs", nargs="*")
    ap.add_argument("--out", default="bridge-viz-out")
    ap.add_argument("--no-video", action="store_true")
    ap.add_argument("--size", default="1920x1080", choices=["1280x720", "1920x1080", "2560x1440"])
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--combine", action="store_true")
    a = ap.parse_args()
    inputs = a.inputs or default_skse_dirs()
    if not inputs:
        print("No input given and no SKSE log folder found. Drag a log / folder / .zip onto this tool, or pass a path.")
        return 2
    tmp = tempfile.mkdtemp(prefix="bridge-viz-")
    try:
        sessions = collect(inputs, tmp)
        if not sessions:
            print("No SKSE-Bridge logs found in:", *inputs, sep="\n  ")
            return 2
        print("%d session(s) found" % len(sessions))
        all_events, base = [], 0.0
        for s in sessions:
            events, source = load_events(s)
            if not events:
                print("  %s: empty, skipped" % s["name"])
                continue
            d = os.path.join(a.out, s["name"])
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "trace.jsonl"), "w", encoding="utf-8") as f:
                f.write("\n".join(json.dumps(e, separators=(",", ":")) for e in events) + "\n")
            open(os.path.join(d, "report.txt"), "w", encoding="utf-8").write(report(s, events, source))
            write_html(os.path.join(d, "replay.html"), events)
            print("  %s  [%s]  %d events -> %s" % (s["name"], source, len(events), d))
            if not a.no_video:
                print("    " + record(os.path.join(d, "replay.html"), os.path.join(d, "replay.webm"), a.size, a.speed))
            end = max(e["t"] for e in events)
            for e in events:
                c = dict(e)
                c["t"] = round(c["t"] + base, 3)
                all_events.append(c)
            base += end + 3000.0
        if a.combine and all_events:
            d = os.path.join(a.out, "_combined")
            os.makedirs(d, exist_ok=True)
            for i, e in enumerate(all_events, 1):
                e["seq"] = i
            write_html(os.path.join(d, "replay.html"), all_events)
            print("combined ->", d)
            if not a.no_video:
                print("    " + record(os.path.join(d, "replay.html"), os.path.join(d, "replay.webm"), a.size, a.speed))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    rc = main()
    if getattr(sys, "frozen", False):          # .exe: keep the window open after a drag-and-drop
        try:
            input("\nDone. Press Enter to close.")
        except EOFError:
            pass
    sys.exit(rc)
