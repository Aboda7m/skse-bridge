# TEST PLAN

I cannot run Skyrim. Everything under "In-game" is for you to run; send me `SKSE-Bridge.log` and `skse64.log`.

## Already done (synthetic, not Skyrim)

Harness: a mock `SkyrimSE.exe` with version resource 1.7.99.0, a mock legacy DLL that follows the
NG `539d4ce` start-up sequence through its own import table, a hand-made format-2 "1.6.1170" file and
a format-5 "1.7.99" file. Run under Wine 9 with a clang/mingw build of the host.

| Case | Result |
|---|---|
| Translate on | mock plugin saw 1.6.1170.0, classed AE, opened the 1.6.1170 file, received the private table, resolved IDs to the **1.7.99** offsets; `SKSEPlugin_Load` returned true |
| Overlay file | 2 valid rules applied, 2 bad lines reported |
| Translate off (negative control) | mock plugin saw 1.7.99.0, classed SE, died on "failed to open address library file" |
| 1.6.1170 library missing | plugin skipped before load, reason logged |
| `dry_run=1` | inspected and logged, not loaded |
| Logged SHA-256 | equals `sha256sum` of the file |

Overlay and poisoning re-run after the per-ID table change: 4 translated + 2 poisoned without overlay, 6 translated with it; missing-ID report lists exactly the 2.

Stage 4 (same harness): known remaps skipped quietly when the legacy library lacks those IDs; `.check`
report lists translated, mid-function and MISSING IDs; a read through a poisoned ID produced
`[fault] ... inside a hosted plugin` with `rax holds a poisoned address: missing Address Library ID 441529`
and a stack; an MSVC-ABI test DLL (built with clang `x86_64-windows-msvc`) with `class PickPocketInput :
public RE::MenuEventHandler` produced `RTTI: INCOMPATIBLE VTABLE ... 6 slots`. Stage 5 (harness, mock SKSE that skips a second listener for the same handle, like skse64): two hosted
plugins registering for "SKSE". `messaging_fanout=0`: second registration skipped, only the first plugin
received message 8 (reproduces the in-game failure). `messaging_fanout=1`: one SKSE registration, both
plugins received message 8. Stage 6 (harness): static-initializer plugin dies with the previous build (log ends at `lineage`) and
loads with the new one (`early: ... imports bound -> patched before DllMain`). Stage 7 (harness): hosted consumer/provider pair; previous build reproduces CNO's "Failed to register
messaging listener" and the provider's undelivered broadcast; new build routes it and `GetPluginInfo` finds
the provider. Tail-call attribution verified (registration from `SKSEPlugin_Load` attributed correctly).

Not covered: real Windows
loader ordering (logged per load), A-variant version queries (no mock caller).

Not covered: the serialization warning path (mock has no serialization).

Not covered: the C++
exception type/`what()` decoding (needs an MSVC-built thrower) and SkyrimSE.exe frame → ID mapping on
the real exe.

Not covered: the MSVC build, the SEH path, a real CommonLib plugin, the real SKSE loader, real Address Library files.

## In-game tiers

- **T0 module mapped**: log line `T0 LoadLibraryW -> <non-null>`.
- **T1 load returns true**: `T1 SKSEPlugin_Load returned true`.
- **T2 lookups resolve**: preflight line reads `392696 of 428461 legacy IDs exist in the installed library` (1.7.99) or `392538` (1.7.104); log shows `VerQueryValueW ... (spoofed)` and `OpenFileMappingW ... -> private table`; no "Failed to find the id" dialog; no `CreateFileMappingW ... REFUSED`.
- **T3 behaviour correct**: your probes for the plugin's observable effect.
- **T4 soak**: long session, save, quit, load.

## Evidence the original DLL ran unchanged

- `sha256:` in the log equals your archived hash.
- `pdb: guid=... age=...` matches your PDB.
- `module path:` is inside the legacy folder.

## Negative controls

1. Same DLL directly in `Data\SKSE\Plugins` on 1.7.99, host removed: record the failure (SKSE rejection or the plugin's dialog).
2. Host installed, `Legacy\` empty: log ends at `legacy plugins: 0`; game unaffected.
3. `[Translate] enabled=0`: expect the plugin's own Address Library dialog.

## Investigating a plugin that stops working

1. Run once with the defaults. In `SKSE-Bridge.log` look for `RTTI: INCOMPATIBLE VTABLE` and `[fault]`.
2. Reproduce the failure in game, then read the `[fault]` lines: `<plugin>.dll+0xRVA` is in your code
   (map it with your build's .map/.pdb); `SkyrimSE.exe+0x... (ID n+0x..)` is the game function.
3. Put the IDs your plugin uses into `Legacy\compat\<plugin>.check` and read the `check:` lines.
4. Run `tools\check_plugin_source.py` on the plugin source.
5. No `[fault]` and no RTTI warning yet the plugin misbehaves: likely a moved member read without an
   exception (wrong data, not a crash). The source checker is the tool for that case.

## First run, in order

1. `dry_run=1`. Check `skse64.log` shows SKSE-Bridge loaded and does **not** list the legacy DLL. Send both logs.
2. `dry_run=0`. Tiers T0–T2 from the log, then T3/T4.


## Patch audit / guard (0.4.0)

Done (Wine, synthetic): see DESIGN Stage 8 "Harness". In game, with Improved Camera SE 1.1.2 in `Legacy\`:
1. Load a save that crashed before. Expected `[patch] ... BLOCKED` for ID 19165+0x7B, 41996+0x2F4, 37356+0xB64
   (if their bytes are not what the hook expects) and for the TogglePOVHandler slot 4.
2. If the save loads: IC works except body-indoors fix, smooth animation transitions, horse look-down fix 1
   and POV-toggle tracking. Report what else misbehaves.
3. Negative control: `patch_guard=0` must reproduce the crash; the `[patch] ... UNSAFE` lines show which.
4. Every other hosted plugin: `[patch] summary` should show 0 blocked for plugins that worked before.


## 0.5.0 in game

1. Improved Camera SE: `[patch] summary` should read 3 blocked (TESObjectCell 19165+0x7B, SmoothAnimation
   41996+0x2F4, TogglePOVHandler slot 4); HorseLookingDownFix1 now `WARNING ... short jump ... is there`.
2. Co-save: host two plugins that use the co-save. Save, quit to desktop, reload. Log: `[cosave] save: <plugin>
   wrote N record(s)` and `[cosave] load: <plugin> got N record(s), read N` for each. Check the plugins' own data.
3. Remove one of them, load and save, put it back, load: its data must come back (`kept ... record(s)` lines).
4. Presented path: a plugin with an ini next to `Data\SKSE\Plugins\`: its settings take effect; the log line
   `module path presented to the plugin` names that path.


## 0.6.0 in game

Same SKSE-Bridge.dll on each game version you have (1.6.640, 1.6.1170, 1.7.x):
1. `target:` line per plugin: built-for version, game version, TRANSLATED or loaded natively, and why.
2. A plugin built for the running game must say `loaded natively` and behave exactly as without the bridge.
3. A 1.6.1170 plugin on 1.7.x: as in 0.5.0.
4. A 1.6.640 plugin: put it in `Legacy\`; if its line does not say 1.6.640, add `[Targets] Name.dll=1.6.640`.
