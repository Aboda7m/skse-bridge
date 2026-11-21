#!/usr/bin/env python3
"""Turn SKSE-Bridge-trace.jsonl (from a --trace=y build) into charts and a video.

    python tools/trace_viz.py <trace.jsonl | folder> [--out trace-out] [--video] [--seconds 30] [--fps 20]

Writes into --out:
    lookups.csv        every inferred ID lookup: time, plugin, ID, translation status, probes used
    overview.png       lookups over time, rate, ID-space scatter, status per plugin
    timeline.png       when each plugin was loaded, with patch / fault markers
    translation.mp4    (--video) the ID space lighting up as plugins resolve IDs, one pixel per Address Library ID

How lookups are inferred: the bridge records every read a plugin makes in its ID table ("probe": entry index + the ID
stored there). A plugin finds an ID with a binary search, so each lookup starts at the same middle entry and ends on the
entry that was found. The most common probe entry of a plugin is that middle entry; it marks the start of a lookup, and
the last probe before the next start is the ID that was resolved. This is an inference, not an exact call log.

Needs: python 3.8+, numpy, matplotlib; ffmpeg on PATH for --video. Trace times are milliseconds since the host started.
"""
import argparse
import collections
import csv
import json
import math
import os
import re
import shutil
import subprocess
import sys

try:
    import numpy as np
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError as exc:  # pragma: no cover
    sys.exit("trace_viz.py needs numpy and matplotlib: pip install numpy matplotlib  (%s)" % exc)

PALETTE = ["#4cc9f0", "#f72585", "#b8f35c", "#ffb703", "#9b5de5", "#00f5d4", "#ff6d00", "#c8b6ff"]
BG, FG, DIM = "#0d1117", "#e6edf3", "#7d8590"
STATUS_COLOR = {"translated": None, "remapped": "#ffd166", "missing": "#ff3b3b", "unknown": None}


# ----------------------------------------------------------------------------------------------- loading
def load_events(path):
    events = []
    bad = 0
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                bad += 1  # a crash can leave a truncated last line
    if bad:
        print("note: skipped %d unreadable line(s)" % bad)
    events.sort(key=lambda e: (e.get("t", 0), e.get("seq", 0)))
    return events


def load_status_tables(folder, events):
    """plugin -> (csv_mode, {id: status}). Missing file -> (None, {})."""
    out = {}
    for e in events:
        if e.get("ev") != "table" or not e.get("csv"):
            continue
        p = os.path.join(folder, e["csv"])
        if not os.path.isfile(p):
            continue
        m = {}
        with open(p, newline="", encoding="utf-8") as f:
            for row in csv.DictReader(f):
                m[int(row["id"])] = row["status"]
        out[e["plugin"]] = (e.get("csv_mode", 1), m)
    return out


def infer_lookups(events, tables):
    probes = [e for e in events if e.get("ev") == "probe"]
    by_plugin = collections.defaultdict(list)
    for e in probes:
        by_plugin[e["plugin"]].append(e)
    first_entry = {}
    for plugin, lst in by_plugin.items():
        first_entry[plugin] = collections.Counter(p["entry"] for p in lst).most_common(1)[0][0]

    lookups = []
    open_cluster = {}

    def flush(key):
        c = open_cluster.pop(key, None)
        if not c:
            return
        plugin = key[0]
        last = c[-1]
        mode, table = tables.get(plugin, (None, {}))
        if mode is None:
            status = "unknown"
        else:
            status = table.get(last["id"], "translated")
        lookups.append({"t": last["t"], "plugin": plugin, "id": last["id"], "status": status, "probes": len(c)})

    for p in probes:
        key = (p["plugin"], p.get("tid", 0))
        if p["entry"] == first_entry[p["plugin"]]:
            flush(key)
        open_cluster.setdefault(key, []).append(p)
    for key in list(open_cluster):
        flush(key)
    lookups.sort(key=lambda r: r["t"])
    return lookups


class Trace:
    def __init__(self, path):
        self.path = path
        self.folder = os.path.dirname(os.path.abspath(path))
        self.events = load_events(path)
        self.tables = load_status_tables(self.folder, self.events)
        self.lookups = infer_lookups(self.events, self.tables)
        self.plugins = []
        for e in self.events:
            if e.get("plugin") and e["plugin"] not in self.plugins:
                self.plugins.append(e["plugin"])
        self.color = {p: PALETTE[i % len(PALETTE)] for i, p in enumerate(self.plugins)}
        self.t_end = max([e.get("t", 0) for e in self.events] or [0])
        self.spans = {}
        for e in self.events:
            if e.get("ev") == "plugin_begin":
                self.spans[e["plugin"]] = [e["t"], None]
            elif e.get("ev") == "plugin_end" and e["plugin"] in self.spans:
                self.spans[e["plugin"]][1] = e["t"]
        self.marks = [e for e in self.events if e.get("ev") in ("patch", "fault")]

    def summary(self):
        lines = ["%s: %d events, %d inferred lookups, %.1f s of trace" % (os.path.basename(self.path), len(self.events), len(self.lookups), self.t_end / 1000.0)]
        for p in self.plugins:
            lk = [r for r in self.lookups if r["plugin"] == p]
            uniq = {r["id"]: r["status"] for r in lk}
            c = collections.Counter(uniq.values())
            lines.append("  %-34s %6d lookups, %5d distinct IDs  %s" % (p, len(lk), len(uniq), ", ".join("%s=%d" % kv for kv in sorted(c.items()))))
        faults = [m for m in self.marks if m["ev"] == "fault"]
        for m in faults:
            mid = re.search(r"missing Address Library ID (\d+)", m.get("detail", ""))
            lines.append("  FAULT in %s at t=%.0f ms%s" % (m.get("plugin", "?"), m["t"], " (missing ID %s)" % mid.group(1) if mid else ""))
        return "\n".join(lines)


# ----------------------------------------------------------------------------------------------- charts
def style_axes(ax, title):
    ax.set_facecolor(BG)
    ax.set_title(title, color=FG, fontsize=11, loc="left")
    ax.tick_params(colors=DIM, labelsize=8)
    for s in ax.spines.values():
        s.set_color("#30363d")


def write_lookups_csv(tr, out):
    with open(os.path.join(out, "lookups.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["t_ms", "plugin", "id", "status", "probes"])
        for r in tr.lookups:
            w.writerow(["%.3f" % r["t"], r["plugin"], r["id"], r["status"], r["probes"]])


def chart_overview(tr, out):
    fig, axs = plt.subplots(2, 2, figsize=(15, 8.5), facecolor=BG)
    (a1, a2), (a3, a4) = axs
    for p in tr.plugins:
        lk = [r for r in tr.lookups if r["plugin"] == p]
        if not lk:
            continue
        t = np.array([r["t"] for r in lk]) / 1000.0
        a1.step(t, np.arange(1, len(t) + 1), where="post", color=tr.color[p], label=p, lw=1.6)
    style_axes(a1, "Cumulative ID lookups per plugin")
    a1.set_xlabel("seconds", color=DIM, fontsize=8)
    a1.legend(facecolor=BG, labelcolor=FG, fontsize=7, framealpha=0.6)

    if tr.lookups:
        t = np.array([r["t"] for r in tr.lookups]) / 1000.0
        bins = max(10, min(200, int(math.ceil(t.max() - t.min() + 1) * 4)))
        data = [np.array([r["t"] for r in tr.lookups if r["plugin"] == p]) / 1000.0 for p in tr.plugins]
        a2.hist([d for d in data if len(d)], bins=bins, stacked=True, color=[tr.color[p] for p, d in zip(tr.plugins, data) if len(d)])
    style_axes(a2, "Lookup rate (stacked by plugin)")
    a2.set_xlabel("seconds", color=DIM, fontsize=8)

    for p in tr.plugins:
        lk = [r for r in tr.lookups if r["plugin"] == p]
        if not lk:
            continue
        a3.scatter([r["t"] / 1000.0 for r in lk], [r["id"] for r in lk], s=3, color=tr.color[p], alpha=0.5, linewidths=0)
    bad = [r for r in tr.lookups if r["status"] == "missing"]
    if bad:
        a3.scatter([r["t"] / 1000.0 for r in bad], [r["id"] for r in bad], s=14, color=STATUS_COLOR["missing"], marker="x", linewidths=0.8, label="ID missing in installed library")
        a3.legend(facecolor=BG, labelcolor=FG, fontsize=7, framealpha=0.6)
    style_axes(a3, "Which IDs, when (Address Library ID vs time)")
    a3.set_xlabel("seconds", color=DIM, fontsize=8)
    a3.set_ylabel("ID", color=DIM, fontsize=8)

    names = [p for p in tr.plugins if any(r["plugin"] == p for r in tr.lookups)]
    bottoms = np.zeros(len(names))
    for status, color in (("translated", "#3fb950"), ("remapped", "#ffd166"), ("missing", "#ff3b3b"), ("unknown", "#8b949e")):
        vals = []
        for p in names:
            uniq = {r["id"]: r["status"] for r in tr.lookups if r["plugin"] == p}
            vals.append(sum(1 for s in uniq.values() if s == status))
        if sum(vals):
            a4.barh(range(len(names)), vals, left=bottoms, color=color, label=status)
            bottoms += np.array(vals)
    a4.set_yticks(range(len(names)))
    a4.set_yticklabels(names, color=FG, fontsize=8)
    a4.invert_yaxis()
    style_axes(a4, "Distinct IDs used, by translation result")
    if len(names):
        a4.legend(facecolor=BG, labelcolor=FG, fontsize=7, framealpha=0.6)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "overview.png"), dpi=110, facecolor=BG)
    plt.close(fig)


def chart_timeline(tr, out):
    fig, ax = plt.subplots(figsize=(15, 1.2 + 0.6 * max(1, len(tr.plugins))), facecolor=BG)
    for i, p in enumerate(tr.plugins):
        s = tr.spans.get(p)
        if s:
            end = s[1] if s[1] is not None else tr.t_end
            ax.barh(i, (end - s[0]) / 1000.0, left=s[0] / 1000.0, color=tr.color[p], height=0.55)
        lk = [r["t"] / 1000.0 for r in tr.lookups if r["plugin"] == p]
        if lk:
            ax.plot(lk, [i] * len(lk), "|", color="white", markersize=6, alpha=0.35)
    for m in tr.marks:
        if m["ev"] == "patch" and m.get("verdict") not in ("blocked", "warned", "unsafe"):
            continue
        p = m.get("plugin")
        if p in tr.plugins:
            bad = m["ev"] == "fault" or m.get("verdict") in ("blocked", "unsafe")
            ax.plot(m["t"] / 1000.0, tr.plugins.index(p), "v", color="#ff3b3b" if bad else "#ffd166", markersize=8)
    ax.set_yticks(range(len(tr.plugins)))
    ax.set_yticklabels(tr.plugins, color=FG, fontsize=8)
    ax.invert_yaxis()
    style_axes(ax, "Plugin load timeline (bars = load window, ticks = ID lookups, triangles = blocked/warned patches and faults)")
    ax.set_xlabel("seconds", color=DIM, fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "timeline.png"), dpi=110, facecolor=BG)
    plt.close(fig)


# ----------------------------------------------------------------------------------------------- video
def warp_times(times, gap_cap):
    """Map trace time -> animation time, squeezing idle gaps longer than gap_cap down to gap_cap."""
    ts = sorted(set(times))
    warped = {}
    acc = 0.0
    prev = None
    for t in ts:
        if prev is not None:
            acc += min(t - prev, gap_cap)
        warped[t] = acc
        prev = t
    return warped, acc


def render_video(tr, out, seconds, fps, size, gap_cap):
    if not shutil.which("ffmpeg"):
        sys.exit("--video needs ffmpeg on PATH")
    if not tr.lookups:
        sys.exit("no lookups in this trace (was [Trace] lookups=1 and did a plugin map its table?)")
    w_px, h_px = size
    max_id = max(r["id"] for r in tr.lookups)
    side = int(math.ceil(math.sqrt(max_id + 1)))

    ev_times = [r["t"] for r in tr.lookups] + [m["t"] for m in tr.marks] + [s[0] for s in tr.spans.values()]
    warped, total = warp_times(ev_times, gap_cap)
    total = max(total, 1e-6)
    frames = int(seconds * fps)
    # frame k covers animation time [k, k+1) * total / frames
    pos = [(warped[r["t"]] / total) * frames for r in tr.lookups]

    dpi = 100
    fig = plt.figure(figsize=(w_px / dpi, h_px / dpi), dpi=dpi, facecolor=BG)
    ax_map = fig.add_axes([0.02, 0.06, 0.58, 0.88])
    ax_map.set_facecolor("#05070a")
    ax_map.set_xticks([]); ax_map.set_yticks([])
    for s in ax_map.spines.values():
        s.set_color("#30363d")
    ax_map.set_title("Address Library ID space  (1 pixel = 1 ID, left→right then down)", color=FG, fontsize=11, loc="left")
    img = np.zeros((side, side, 3), dtype=np.float32)
    im = ax_map.imshow(img, interpolation="nearest", aspect="equal")

    ax_side = fig.add_axes([0.62, 0.06, 0.36, 0.88])
    ax_side.set_facecolor(BG); ax_side.axis("off")
    ax_side.set_xlim(0, 1); ax_side.set_ylim(0, 1)
    title = ax_side.text(0, 0.985, "SKSE-Bridge  ID translation", color=FG, fontsize=15, fontweight="bold", va="top")
    clock = ax_side.text(0, 0.935, "", color=DIM, fontsize=10, va="top")
    lane_texts, lane_bars = {}, {}
    n_pl = max(1, len(tr.plugins))
    lane_h = min(0.10, 0.52 / n_pl)
    for i, p in enumerate(tr.plugins):
        y = 0.88 - i * lane_h
        lane_texts[p] = ax_side.text(0, y, p, color=tr.color[p], fontsize=9, va="top")
        ax_side.add_patch(plt.Rectangle((0, y - 0.045), 1, 0.014, color="#21262d", lw=0))
        lane_bars[p] = ax_side.add_patch(plt.Rectangle((0, y - 0.045), 0, 0.014, color=tr.color[p], lw=0))
    feed_top = 0.88 - n_pl * lane_h - 0.03
    ax_side.text(0, feed_top, "latest lookups", color=DIM, fontsize=9, va="top")
    feed = [ax_side.text(0, feed_top - 0.04 - j * 0.032, "", color=FG, fontsize=8, va="top", family="monospace") for j in range(10)]

    per_plugin_total = collections.Counter(r["plugin"] for r in tr.lookups)
    rgb = {p: np.array(matplotlib.colors.to_rgb(tr.color[p]), dtype=np.float32) for p in tr.plugins}
    red = np.array(matplotlib.colors.to_rgb(STATUS_COLOR["missing"]), dtype=np.float32)
    yellow = np.array(matplotlib.colors.to_rgb(STATUS_COLOR["remapped"]), dtype=np.float32)
    seen_color = np.zeros((side * side, 3), dtype=np.float32)   # dim, permanent
    glow = np.zeros(side * side, dtype=np.float32)              # bright, decays
    glow_color = np.zeros((side * side, 3), dtype=np.float32)
    done = collections.Counter()
    recent = collections.deque(maxlen=len(feed))
    flat = np.zeros((side * side, 3), dtype=np.float32)

    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", "%dx%d" % (w_px, h_px), "-r", str(fps),
           "-i", "-", "-an", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", os.path.join(out, "translation.mp4")]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    nxt = 0
    t_of_frame = total / frames
    inv_warp = sorted((warped[r["t"]], r["t"]) for r in tr.lookups)
    for k in range(frames):
        while nxt < len(tr.lookups) and pos[nxt] < k + 1:
            r = tr.lookups[nxt]
            i = r["id"]
            c = red if r["status"] == "missing" else (yellow if r["status"] == "remapped" else rgb[r["plugin"]])
            seen_color[i] = c * (0.8 if r["status"] not in ("missing", "remapped") else 1.0)
            glow[i] = 1.0
            glow_color[i] = c
            done[r["plugin"]] += 1
            tag = {"missing": "MISSING", "remapped": "remapped"}.get(r["status"], "ok")
            recent.appendleft((r["plugin"], i, tag, c))
            nxt += 1
        glow *= 0.90
        flat = np.maximum(seen_color, glow_color * glow[:, None])
        # bright flash: blend towards white while glow is high
        flat = np.clip(flat + (glow[:, None] ** 3) * 0.35, 0, 1)
        # one ID is one pixel; grow each lit pixel to 2x2 so it stays visible at video resolution
        pic = flat.reshape(side, side, 3)
        big = pic.copy()
        big[:, 1:] = np.maximum(big[:, 1:], pic[:, :-1])
        big[1:, :] = np.maximum(big[1:, :], big[:-1, :])
        im.set_data(big)
        trace_ms = inv_warp[min(len(inv_warp) - 1, max(0, np.searchsorted([w for w, _ in inv_warp], (k / frames) * total)))][1]
        clock.set_text("trace time %.2f s    frame %d/%d" % (trace_ms / 1000.0, k + 1, frames))
        for p in tr.plugins:
            lane_bars[p].set_width(done[p] / per_plugin_total[p] if per_plugin_total[p] else 0)
            lane_texts[p].set_text("%s   %d" % (p, done[p]))
        for j, t in enumerate(feed):
            if j < len(recent):
                pl, i, tag, c = recent[j]
                t.set_text("%-22s %7d  %s" % (pl[:22], i, tag))
                t.set_color(tuple(c) if tag != "ok" else FG)
            else:
                t.set_text("")
        fig.canvas.draw()
        proc.stdin.write(np.asarray(fig.canvas.buffer_rgba()).tobytes())
        if k % 50 == 0:
            print("frame %d/%d" % (k, frames), end="\r", flush=True)
    proc.stdin.close()
    proc.wait()
    plt.close(fig)
    print("\nwrote", os.path.join(out, "translation.mp4"))


# ----------------------------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace", help="SKSE-Bridge-trace.jsonl or the folder that contains it")
    ap.add_argument("--out", default="trace-out", help="output folder (default: trace-out)")
    ap.add_argument("--video", action="store_true", help="also render translation.mp4 (needs ffmpeg)")
    ap.add_argument("--seconds", type=float, default=30, help="video length (default 30)")
    ap.add_argument("--fps", type=int, default=20)
    ap.add_argument("--size", default="1280x720")
    ap.add_argument("--gap-cap", type=float, default=250.0, help="idle gaps longer than this many trace-milliseconds are squeezed to it in the video")
    args = ap.parse_args()

    path = args.trace
    if os.path.isdir(path):
        path = os.path.join(path, "SKSE-Bridge-trace.jsonl")
    if not os.path.isfile(path):
        sys.exit("not found: %s" % path)
    os.makedirs(args.out, exist_ok=True)

    tr = Trace(path)
    print(tr.summary())
    write_lookups_csv(tr, args.out)
    chart_overview(tr, args.out)
    chart_timeline(tr, args.out)
    print("wrote lookups.csv, overview.png, timeline.png ->", args.out)
    if args.video:
        w, h = (int(x) for x in args.size.lower().split("x"))
        render_video(tr, args.out, args.seconds, args.fps, (w, h), args.gap_cap)


if __name__ == "__main__":
    main()
