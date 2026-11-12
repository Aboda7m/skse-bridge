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

## First run, in order

1. `dry_run=1`. Check `skse64.log` shows SKSE-Bridge loaded and does **not** list the legacy DLL. Send both logs.
2. `dry_run=0`. Tiers T0–T2 from the log, then T3/T4.
