# DESIGN

Tags: **[V]** verified in source (file:line), **[T]** observed in the synthetic harness, **[A]** assumption, **[U]** unknown.
`CL` = CommonLibSSE-NG `539d4ce` (MIT), the revision the legacy plugins were built with. `NEW` = NG `9955ae3`.

## Why a legacy plugin fails on 1.7.99 without help

| Step in the legacy plugin | Evidence | Result on 1.7.99 |
|---|---|---|
| Reads exe `ProductVersion` string | [V] `CL/src/REL/Version.cpp:7-33` | 1.7.99.0 |
| Minor 6 → AE, anything else → SE | [V] `CL/include/REL/Module.h:339-348` | multi-runtime build: classed **SE** |
| Opens `versionlib-<ver>.bin` (AE, format 2) or `version-<ver>.bin` (SE, format 1) | [V] `CL/include/REL/ID.h:317-323` | SE path: file missing → fatal |
| Header format must match | [V] `ID.h:256-267` | AE-only build: 1.7.99 file is format 5 → fatal. Format 5: [V] `NEW/src/REL/IDDB.cpp:193-213` |
| Only then tries the shared table `CommonLibSSEOffsets-v2-<ver>` | [V] `CL/src/REL/ID.cpp:99-113` | never reached |

So intercepting the mapping alone is not enough; the version must be adapted first.

## Stages

**Stage 1 — loader** (`src/main.cpp`, `src/pe.cpp`). INI, on-disk inspection, `LoadLibraryW`,
`SKSEPlugin_Load` under SEH (MSVC builds), structured log. Translates nothing.
With `[Translate] enabled=0` this is all that runs.

**Stage 2 — per-module import interposer** (`src/interpose.cpp`). Patches four import slots of the
legacy module only:

| Function | Behaviour | Why |
|---|---|---|
| `GetFileVersionInfoW` | forwarded; remembers buffers that describe the game exe | to know which `VerQueryValueW` calls to answer |
| `VerQueryValueW` | for those buffers, `ProductVersion`/`FileVersion`/`\` return `legacy_runtime` | fixes classification and file name |
| `OpenFileMappingW` | name `CommonLibSSEOffsets-v2-*` → duplicate handle of the host's private, unnamed table | new offsets |
| `CreateFileMappingW` | same name → refused | no silent fallback to 1.6.1170 offsets |

Everything else is forwarded. Every interposed call is logged. Calls are attributed to a module by
return address, so only hosted DLLs are affected. [T]

The SKSE interface passed in is a copy with `runtimeVersion` set to `legacy_runtime`
(`spoof_skse_runtime`). Layout: [V] `CL/include/SKSE/Impl/Stubs.h:26-36`.

**ID numbering, measured on the real files** (`tools/compare_address_library.py`, run 2026-11-12 on
Address Library All in One 1.7.104.0 v13): [V]

| | 1.6.1170 | 1.7.99 | 1.7.104 |
|---|---|---|---|
| format | 2 | 5 | 5 |
| IDs present | 428,461 | 435,154 | 435,162 |
| highest ID | 510,897 | 565,072 | 565,758 |

- 392,696 of 428,461 1.6.1170 IDs (91.7%) exist in 1.7.99; 35,765 do not.
- For IDs present in both, 97.6% of neighbouring pairs keep an identical byte gap (1.6.1170→1.6.1179,
  a patch known to keep numbering: 99.5%). Renumbering would put this near 0%. Conclusion: same
  numbering; the 1.7.99 library itself is the ID translation. New IDs start at 522,615, continuing
  after 1.6.1179's highest (522,614).
- Of the 35,765 dropped IDs, only 355 sit in a block whose size is unchanged, so inferring their new
  offsets from neighbours is not worthwhile. Not implemented.
- Format 5 header confirmed on these files: `format=5`, `version`, `name[64]`, `pointerSize=8`,
  `dataFormat=0`, `count`, then `count` × u32 at byte 96, no trailing bytes.

**Private table** (`src/idtable.cpp`). Built at start-up from both installed files:
- one row per ID of the 1.6.1170 library, same order, so the row count equals the `address_count`
  the legacy plugin reads from that header; offset = the installed library's offset for that ID;
- IDs the installed library lacks get `0x0BAD000000000000 | id`: base + that is non-canonical, so
  any use faults at once and the crash address shows the ID; never a silently wrong neighbour;
- overlay rules (`legacyID = newID`) then redirect individual rows.
Readers:
- format 5: header + dense `u32[id]`, 0 = absent. [V] `NEW/include/REL/IDDB.h:222-251`, `IDDB.cpp:156-164,274-301`. `pointerSize`/`dataFormat` meaning: [U], logged only.
- format 1/2: packed stream. [V] `CL/include/REL/ID.h:256-282,335-412`.
- Shape expected by the legacy code: sorted `{u64 id, u64 offset}`, exactly `address_count` rows taken
  from the **1.6.1170** file header. [V] `CL/src/REL/ID.cpp:108-110`. The host reads that header (header only) to learn the row count, then truncates or pads.

**Stage 3 — ID overlay.** `Legacy\compat\<plugin>.ids`, lines `legacyID = newID`, `#`/`;` comments.
Applied to that plugin's table before fitting. No code thunks.

## Known limits

1. No struct-layout or vtable translation. Out of scope by design.
2. `DllMain` timing: the patch lands after `LoadLibraryW` returns. NG initializes `Module`/`IDDatabase`
   lazily on first `get()` [V] `CL/src/SKSE/API.cpp:83-84`, but a namespace-scope `REL::Relocation` in the
   plugin would run earlier and hit the unpatched path (fatal dialog).
3. Lookup has no exact-match check on non-VR builds [V] `CL/include/REL/ID.h:175-200`. Because the
   table carries every legacy ID, this only matters for IDs that were never in the 1.6.1170 library;
   those resolve to the next higher legacy ID, as they would on 1.6.1170.
4. Dropped IDs (8.3%) fault when used. Only an overlay with a known replacement ID helps.
5. Same ID number does not guarantee the same function signature or behaviour; offsets within a
   function (`ID + 0x22` hook sites) may have moved.
6. `report_and_fail` inside a legacy plugin terminates the process [A]; SEH does not catch it. The
   preflight exists to avoid reaching it for known causes.
7. The plugin believes it runs on 1.6.1170 wherever it asks.
8. Identity collapse: hosted plugins use the host's plugin handle (messaging sender, co-save UID).
9. Host plugin declaration (`SKSEPlugin_Version`): the 1.7.99 loader's requirements were not inspected. [U]
10. SKSE not scanning `Plugins\Legacy\` recursively: [A].
