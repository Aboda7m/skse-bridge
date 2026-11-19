# SKSE-Bridge

An experimental compatibility layer for running selected legacy SKSE plugins on newer Skyrim runtimes.

**Current version: 0.3.0**

## What is SKSE-Bridge?

Some SKSE plugins were built for Skyrim 1.6.1170 and stop working when the game updates. SKSE-Bridge is an
SKSE plugin that loads those plugins on Skyrim 1.7.99 / 1.7.104 **without modifying or recompiling them**
(the DLL on disk stays byte-for-byte identical), and adapts only as much as is needed for them to start.

This is an experimental project, not a universal compatibility solution. Whether a plugin works depends on
how it interacts with Skyrim. Some plugins will still need to be rebuilt by their authors.

## Why was it made?

Every Skyrim runtime update can break SKSE plugins until their authors publish compatible builds. That
leaves users waiting, or without plugins whose authors no longer maintain them. SKSE-Bridge explores
whether a separate compatibility layer can reduce that dependency, and lets other developers inspect, test
and improve the approach.

## Requirements

- Skyrim Special Edition **1.7.99** or **1.7.104**
- SKSE matching your installed runtime
- Address Library for SKSE Plugins: **All in One (1.7.104.0) v13**

One Address Library download is enough. The All in One package includes the 1.6.1170 database the legacy
plugins look for (`versionlib-1-6-1170-0.bin`) next to the one for your runtime. Do not install a separate
1.6.1170 package. SKSE-Bridge never copies or redistributes Address Library files; it reads the ones you
installed.

## Installation

1. Install SKSE for your runtime.
2. Install Address Library All in One (1.7.104.0) v13.
3. Install SKSE-Bridge with your mod manager, or manually:
   ```
   Data\SKSE\Plugins\SKSE-Bridge.dll
   Data\SKSE\Plugins\LegacyHost.ini
   ```
4. Put the unmodified 1.6.1170 plugin DLLs you want to try in:
   ```
   Data\SKSE\Plugins\Legacy\
   ```
   Only the DLL moves. Leave the plugin's ini files and data folders where the mod installs them
   (`Data\SKSE\Plugins\...`); since 0.5.0 the hosted plugin is told it lives there.
5. Launch the game and check whether the plugins work (see Troubleshooting).

**Important:** only put plugins in `Legacy\` that you want SKSE-Bridge to host. Plugins that SKSE should
load normally must stay in `Data\SKSE\Plugins\`, outside the `Legacy` folder.

## Tested plugins

Tested on Skyrim 1.7.104 with SKSE 2.3.1.

| Plugin | Status |
|---|---|
| RTL-SUPPORT-NG | Loads and applies its XML translations |
| PickPocketRedone-NG | Loads and works |
| CompassNavigationOverhaul + InfinityUI | Compatibility work added in 0.3.0; in-game testing still in progress |
| Improved Camera SE 1.1.2 | Loads and runs since 0.4.0; a few features are disabled by the patch guard (see log) |

Other plugins have not been tested. Results vary with each plugin's implementation.

## Limitations

SKSE-Bridge translates **Address Library IDs**. It cannot change anything that the plugin compiled into its
own code.

- **IDs missing in 1.7.99.** 35,765 of the 428,461 IDs in the 1.6.1170 library (about 8%) do not exist in the
  1.7.99 library. A plugin that uses one will crash at a deliberately invalid address, and the log names the
  ID. The full list is written to `SKSE-Bridge-missing-ids.txt` next to the log. If you know the replacement
  ID, add `legacyID = newID` to `Legacy\compat\<plugin>.ids`.
- **Changed structures and virtual tables.** Some game structures changed in 1.7.99 (for example
  `SkyrimVM`, `PlayerCharacter` runtime data, and the `MenuEventHandler` / `PlayerInputHandler`
  interfaces). Plugins whose code depends on them need to be rebuilt. The bridge can detect some of these
  cases and logs them, but cannot fix them.
- **Changed functions.** An ID that still exists may point at a function whose signature or behaviour
  changed. Hooks placed at `ID + offset` (an offset inside the function, read off 1.6.1170 machine code)
  land on other code when 1.7.x compiled that function differently. Since 0.4.0 the bridge checks every
  such write and refuses the ones that miss (`patch_guard`): the plugin then loads without that feature
  instead of crashing the game later. The plugin still needs a rebuild for full function.
- **Shared SKSE identity.** Hosted plugins share the bridge's SKSE plugin handle. Messaging is handled by the
  bridge (hosted plugins can message each other, for example CompassNavigationOverhaul and InfinityUI), but
  plugins loaded by SKSE itself cannot reach a hosted plugin by name.
- **Co-save.** Since 0.5.0 the bridge owns one co-save slot (`'SKBR'`) and gives each hosted plugin its own
  sub-space in it, so several hosted plugins can save. Co-save data a hosted plugin wrote with 0.4.0 or
  earlier (or with `cosave_multiplex=0`) was stored under that plugin's own ID and is not read by the new
  layout; expect that plugin to start fresh once.
- **Supported lineage.** Only plugins built with the CommonLibSSE-NG lineage are understood. Others are
  skipped unless `force=1` is set.

## What's new in 0.5.0

- Co-save sub-spaces: every hosted plugin saves and loads its own records inside the bridge's co-save slot.
  Records of a hosted plugin that is missing for a session are kept and written back.
- Hosted plugins see their normal path (`Data\SKSE\Plugins\<name>.dll`), so ini files and data folders
  installed next to that location are found.
- Patch guard refines its check: a 2-byte patch over a short jump, a 6-byte patch over a near conditional
  jump or a virtual call are recognised; per-plugin allow-list `Legacy\compat\<plugin>.patches`.

## What's new in 0.4.0

- Patch audit: every write a hosted plugin makes into `SkyrimSE.exe` (hooks, byte patches, vtable hooks) is
  logged as a `[patch]` line with the Address Library ID, the bytes before and after, and a verdict.
- Patch guard: writes that would land on the wrong game code are refused and logged, so a plugin with
  outdated in-function hook offsets loses those features instead of crashing the game later
  (found investigating Improved Camera SE 1.1.2 on 1.7.104).
- `tools/check_plugin_source.py` also flags `ID + offset` patch sites and vtable hooks on input handlers.

## What's new in 0.3.0

- Better support for plugins that do setup work during DLL initialization, before the bridge could patch them.
- Fixed hosted plugins missing SKSE startup messages such as `kDataLoaded`. This also fixed RTL-SUPPORT-NG
  not initializing its XML translations.
- Hosted plugins can now exchange messages with each other (initial CompassNavigationOverhaul and
  InfinityUI support; in-game testing is in progress).
- Improved logging and diagnostics.

## Configuration

`Data\SKSE\Plugins\LegacyHost.ini` (defaults are fine for most people):

| Setting | Meaning |
|---|---|
| `[General] dry_run` | `1` = inspect and log every legacy DLL, load nothing |
| `[General] plugins` | `*` for every DLL in `Legacy\`, or a comma-separated list of file names |
| `[Translate] enabled` | `0` = plain load with nothing adapted (negative control) |
| `[Translate] force` | `1` = load plugins even when prerequisites are missing |
| `[Diagnostics] verify_ids`, `rtti_scan`, `fault_log` | Extra checks written to the log |
| `[Diagnostics] patch_audit` | Log every write hosted plugins make into the game exe (`[patch]` lines) |
| `[Compatibility] patch_guard` | `1` = refuse writes that would land on the wrong game code (default); `0` = log only |
| `[Compatibility] present_plugin_path` | `1` = hosted DLLs see themselves in `Data\SKSE\Plugins\` (default) |
| `[Compatibility] cosave_multiplex` | `1` = one co-save sub-space per hosted plugin (default) |

Optional per-plugin files in `Legacy\compat\`: `<plugin>.ids` (ID overrides, one `legacyID = newID` per
line), `<plugin>.check` (paste the IDs your plugin uses and the log reports each one) and
`<plugin>.patches` (`allow <ID>+0x<offset>` per line: patch sites you checked that the guard must not refuse).

## Troubleshooting

Main log:

```
Documents\My Games\Skyrim Special Edition\SKSE\SKSE-Bridge.log
```

Check it first to confirm SKSE loaded SKSE-Bridge and to look for errors. Useful lines:

- `T1 SKSEPlugin_Load returned true`: the hosted plugin started.
- `RTTI: INCOMPATIBLE VTABLE`: the plugin implements a game interface whose layout changed.
- `[fault]`: an exception in or through a hosted plugin, with locations. Start here when a plugin stops working.
- `[patch] ... BLOCKED`: a hook or byte patch the plugin tried to place on game code that changed in 1.7.x.
  The bridge refused it; the feature behind it will not work, but the game stays intact.
- `[patch] summary`: per plugin, how many writes were fine, warned, or blocked.

When reporting a problem, please include:

- Skyrim runtime version and SKSE version
- The legacy plugin's name and version
- Your Address Library package version
- The SKSE-Bridge log and any `[fault]` lines

Reports of plugins that work are just as useful as reports of failures.

## Building

```bat
xmake f -m releasedbg
xmake
```

Requires MSVC and C++20. No external packages. Source is in `src/`. For developers there are two helpers in
`tools/`:

- `compare_address_library.py <old.bin> <new.bin> [ids.txt]` checks which IDs a plugin uses exist in the new library.
- `check_plugin_source.py <plugin source dir>` flags code that depends on layouts that changed in 1.7.99.

Do not commit Address Library files; they are not part of this project.

## Development and contributions

SKSE-Bridge is an experimental community project. Inspecting the code, testing plugins, reporting results
and contributing fixes are all welcome. The design notes, the Address Library ID research and the test plan
are in `docs/`.

## Credits

- [SKSE](https://skse.silverlock.org/)
- CommonLibSSE-NG, by Ryan-rsm-McKenzie and contributors (struct layouts and Address Library format used by the legacy plugins)
- Address Library for SKSE Plugins by meh321
