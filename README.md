# SKSE-Bridge — legacy plugin host

An SKSE plugin for Skyrim 1.7.99+ that loads SKSE plugins compiled for 1.6.1170, unchanged
(no source edits, no recompile, identical file hash), and adapts as little as needed for them to start.

**Status: builds; needs in-game verification.** It has never been run inside Skyrim. It was
exercised end to end only in a synthetic harness (a mock exe and a mock legacy DLL that follows the
CommonLibSSE-NG start-up path). See `docs/TEST_PLAN.md`.

## What it does

1. Reads `Data\SKSE\Plugins\LegacyHost.ini`.
2. Inspects each DLL in `Data\SKSE\Plugins\Legacy\` on disk and logs SHA-256, PDB GUID/age, exports,
   imports and its `SKSEPlugin_Version` data.
3. Loads the DLL, then patches **that DLL's own import table** so that
   - it sees the game as 1.6.1170 (and therefore opens the 1.6.1170 Address Library file), and
   - its Address Library ID table is one the host built per ID: every 1.6.1170 ID gets its
     1.7.99 offset from the **installed 1.7.99** Address Library.
   1.7.99 keeps the 1.6.1170 ID numbering (checked on the real files, see `docs/DESIGN.md`), so for
   92% of IDs this is a direct translation. The other 8% do not exist in 1.7.99; they get a poisoned
   offset that faults immediately instead of pointing at the wrong function.
4. Calls its `SKSEPlugin_Load` with a copy of the host's SKSE interface.

Log: `Documents\My Games\Skyrim Special Edition\SKSE\SKSE-Bridge.log`.

## What it cannot do

- **It cannot translate struct layouts or vtable slots.** A legacy plugin that reads or writes a game
  object whose layout changed in 1.7.99 will read garbage or crash. Example reported by another project:
  1.7.99 shifts `RE::SkyrimVM` members from 0x180 onward by 0x10. Papyrus-heavy plugins are at risk.
- It cannot fix IDs whose function changed signature or behaviour, or hooks at `ID + offset` where the
  code around the offset moved.
- Anything the legacy DLL does with versions or IDs in `DllMain` / static initializers runs before the patch.
- 35,765 of the 428,461 IDs in the 1.6.1170 library are not in the 1.7.99 library. A plugin using one
  of them crashes at `SkyrimSE.exe + 0x0BAD000000000000 + id` (non-canonical address). The full list is
  written to `SKSE-Bridge-missing-ids.txt` in the SKSE log folder. If you know the replacement ID, add
  `legacyID = newID` to `Legacy\compat\<plugin>.ids`.
- Check your plugin's IDs before running: `python tools/compare_address_library.py
  versionlib-1-6-1170-0.bin versionlib-1-7-99-0.bin ids.txt`, where `ids.txt` is a grep of
  `REL::ID`/`RELOCATION_ID` from its source.
- Hosted plugins share the host's SKSE identity: messaging sender name and co-save unique ID.
- Only plugins from the CommonLibSSE-NG lineage (shared table `CommonLibSSEOffsets-v2-*`) are understood.
  Others are skipped unless `force=1`.

Expect this to work for simple plugins and to fail for many real ones.

## Requirements (you install these; nothing is bundled)

- SKSE for your runtime.
- Address Library for your runtime: `versionlib-1-7-99-0.bin` or `versionlib-1-7-104-0.bin` (picked from the exe version).
- Address Library for 1.6.1170 as well: `versionlib-1-6-1170-0.bin`. The legacy plugin opens and
  validates this file's header itself. Its offsets are never used.

## Install

```
Data\SKSE\Plugins\SKSE-Bridge.dll
Data\SKSE\Plugins\LegacyHost.ini
Data\SKSE\Plugins\Legacy\<your 1.6.1170 plugin>.dll      <- unchanged
Data\SKSE\Plugins\Legacy\compat\<plugin>.ids             <- optional: "legacyID = newID" per line
```

## Build

```bat
xmake f -m releasedbg
xmake
```

MSVC, C++20, no packages. `dist\SKSE-Bridge.dll` is a convenience test build made with clang
(mingw target); it lacks the SEH guard around `SKSEPlugin_Load`. Prefer your own MSVC build.

Source is in `src\`. Old template content (`view\`, `lib\`, `ClibUtil\`, `xbyak\`, `xmake-requires.lock`)
is no longer used by the build and can be deleted. `Address Library\` is git-ignored: never commit it.
