# Turning bridge logs into a visualisation

Every release build already writes `SKSE-Bridge.log` (game/SKSE version, config, each plugin's lineage, table stats,
patches with verdicts, messaging, shadow-VM redirects, co-save, faults). The log used to be overwritten on every launch; now the
previous one is copied to `SKSE-Bridge-history\SKSE-Bridge-<yyyymmdd-hhmmss>.log` (plus the matching `-trace.jsonl` when a
trace build wrote one) next to it, and the newest 20 sessions are kept.

`tools/bridge_viz.py` (or the `SKSE-Bridge-Viz.exe` built by `tools/build_exe.bat`) turns any of that into, per session:
`replay.html` (interactive, self-contained), `replay.webm` (video), `report.txt` (setup + problems), `trace.jsonl`.

```
python tools\bridge_viz.py                       # your own SKSE folder, current log + history
python tools\bridge_viz.py "C:\tester\SKSE"      # a folder, an SKSE-Bridge*.log, a trace .jsonl or a .zip the tester sent
python tools\bridge_viz.py logs.zip --combine    # also one video that plays every session after each other
python tools\bridge_viz.py log.log --no-video    # html + report only (no browser needed)
```

Video needs `pip install playwright` and an installed Edge/Chrome. A trace build (`--trace=y`, `[Trace] enabled=1 flows=1`) adds exact
ID lookups to the picture; a plain log does not have them (Address Library node stays empty). To ask a tester for data: send the
`SKSE-Bridge-history` folder (or a zip of the SKSE folder) - no trace build needed for everything else.
