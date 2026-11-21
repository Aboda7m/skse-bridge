# Trace logging (test builds only)

Purpose: record, while the game runs, **which Address Library IDs each hosted plugin used and how each one was
translated**, so the log can be turned into charts and a video of the translation happening.

It is **not part of the release DLL**. Two switches, both off by default:

1. Build time: `xmake f -m releasedbg --trace=y` then `xmake`. Without `--trace=y` the code is not compiled in
   (`SKB_TRACE_DO(...)` expands to nothing), so the shipped `dist/SKSE-Bridge.dll` has none of it.
2. Run time: `[Trace] enabled=1` in `LegacyHost.ini`. A trace build with `enabled=0` writes nothing.

## What is recorded

Files, next to `SKSE-Bridge.log` (`Documents\My Games\Skyrim Special Edition\SKSE\`):

| File | Content |
|---|---|
| `SKSE-Bridge-trace.jsonl` | one JSON object per line, `t` = ms since the host started |
| `SKSE-Bridge-trace-table-<plugin>.csv` | translation result per legacy ID: `id,legacy_offset,new_offset,status` (`translated` / `remapped` / `missing`) |

Events (`ev`): `run_start`, `host`, `plugin_begin`, `plan`, `table` (counts + CSV name), `table_open`, `hook`,
`table_view`, `dll_loaded`, `skse_load_begin`, `skse_load_end`, `plugin_end`, `load_phase_end`, `patch`
(blocked/warned/ok writes into the game exe), `fault` (with the missing ID when a poisoned address was used),
`probe`, `heartbeat`, and the flow events below.

### Flow events (`[Trace] flows=1`)

| `ev` | Meaning | Fields |
|---|---|---|
| `shadow_ready` | a shadow class (SkyrimVM / PlayerCharacter) was set up, or could not be | `class`, `ok`, `game_layout`, `why` |
| `shadow_attach` | a hosted plugin was attached to the shadow(s) | `plugin`, `classes`, `layout` |
| `vm_access` | accesses a plugin (or game code) made to a shadowed object that the handler redirected. Batched per member, target and read/write | `plugin` (absent = game code), `class`, `off` (plugin layout), `real` (this runtime), `write`, `target` (`real` / `pending` = object not created yet / `absent` = no such member), `n` total, `d` since last line |
| `msg_listen` | a hosted plugin registered a message listener | `plugin`, `sender`, `internal`, `ok` |
| `msg` | a message delivered to a hosted plugin: `route":"skse"` (fan-out of one SKSE registration) or `"internal"` (plugin to plugin, never reaches SKSE) | `plugin` (receiver), `from`, `type`, `len`, `n`, `d` |
| `msg_out` | a hosted plugin's `Dispatch` forwarded to SKSE | `plugin`, `from`, `to`, `type`, `delivered`, `n`, `d` |
| `cosave` | co-save multiplexer: `register`, `claim`, `save`, `load`, `save_orphans`, `load_orphans`, `revert` | `phase`, `plugin`, `uid`, `records` |

Repeating events (`vm_access`, `msg`, `msg_out`) are written verbatim for the first 8 occurrences, then at most once per
100 ms with the running total `n` and the new count `d`, so totals are exact and only the timing is batched.
What is *not* recorded: anything a plugin computes inside its own code, and the work of the shadow VM beyond the
redirect itself (the bridge only sees the access and where it was sent).

### How "every ID a plugin used" is captured

A hosted plugin looks up IDs itself, by binary search in a table the bridge hands it (a memory mapping), so the
bridge never sees a function call per ID. In a trace build the bridge hooks the plugin's `MapViewOfFile`, marks the
pages of that table `PAGE_GUARD`, and records every read from an exception handler (entry index + the ID stored there),
re-arming the page after each read by single-stepping one instruction. Each read is a `probe` event.

`tools/trace_viz.py` and `trace_replay.html` turn probes into lookups: the first probe of every binary search is the
same middle entry, and the last probe is the entry that was found. That makes lookups an **inference**: counts are
reliable, but an unusual probe pattern could split or merge a lookup. The `table` CSV is exact (it is the table itself).

### Settings (`[Trace]`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 0 | master switch (trace builds only) |
| `lookups` | 1 | record probes. Slow by design (guard-page exceptions); set 0 for events/tables only |
| `patches` | 1 | record writes into the game exe as `patch` events |
| `table_dump` | 1 | 0 none, 1 only IDs that are not plain 1:1 (missing/remapped), 2 every ID (~17 MB per plugin) |

## Visualising

### Graph view and video (`tools/trace_replay.html`)

Open the file in Chrome/Edge/Firefox and pick `SKSE-Bridge-trace.jsonl` (plus the table CSVs). **Graph** shows plugins, the
bridge, the ID table, the shadow VM, real game objects, game code, SKSE and the co-save as nodes; every logged event is a
beam between them (ID lookups green/yellow/red, patches blue/yellow, blocked patches stop at the bridge with a red cross,
VM reads cyan, writes pink, redirects into a dummy grey, messages purple, co-save orange). Click a plugin (or use **Focus**)
for a single-plugin view: the plugin in the centre with its load sequence checklist, lookups, patches, VM accesses and
messages around it, and only its events in the feed. **Record video** renders the canvas straight to a `.webm`
(720p/1080p/1440p, real time at the chosen speed, keep the tab visible); convert with
`ffmpeg -i skse-bridge-trace-graph.webm -c:v libx264 -pix_fmt yuv420p out.mp4` if you need mp4. **ID map** is the old
one-pixel-per-ID view.

```bat
python tools\trace_viz.py "%USERPROFILE%\Documents\My Games\Skyrim Special Edition\SKSE" --out trace-out --video
```

writes `lookups.csv`, `overview.png`, `timeline.png` and `translation.mp4` (one pixel per ID, lighting up as each plugin
resolves it; missing IDs flash red). Needs Python with numpy and matplotlib, plus ffmpeg for `--video`.
Options: `--seconds`, `--fps`, `--size 1920x1080`, `--gap-cap` (idle trace time squeezed in the video).

For a live-looking replay open `tools/trace_replay.html` in a browser, pick the `.jsonl` (and the table `.csv` files), and
screen-record it (Fullscreen button, speed slider, scrub bar).

No game handy? `python tools/make_sample_trace.py trace-out/sample` makes a synthetic trace to try the tools on.

## Limits (read before trusting a video)

- Written without an MSVC build or a Skyrim run. The visualisation tools were tested on synthetic data only; the C++
  was syntax-checked, not run. Check `SKSE-Bridge.log` for `trace: ON` and the trace for `table_view ... "armed":true`.
- Only the bridge's private 16-byte-row table is probed (CommonLibSSE-NG plugins translated by the bridge). Plugins that
  read the shared dense table (commonlib-shared / libxse, `[Targets] ... =native` with shadow) get a `table`/lifecycle
  record but no probes.
- A plugin that reads the table through a mapping view created some other way than `MapViewOfFile` from its own
  imports is not probed.
- Guard-page tracing changes timing. Use it to see *what* is looked up, not to measure plugin start-up speed.
- Up to 32 views and a 131,072-entry probe ring; overflow is counted in `heartbeat.probes_dropped`.
