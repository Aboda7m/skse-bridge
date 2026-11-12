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
2. `DllMain` timing: addressed in Stage 6 (patch from the loader notification, before `DllMain`).
   Falls back to patching after `LoadLibraryW` if the notification is unavailable or imports are not yet
   bound; the log says which.
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

## Stage 4 — beyond ID numbering (2026-11-12)

Source of every 1.7.99 change below: CommonLibSSE-NG `9955ae3`, which marks each with
`RUNTIME_SSE_1_7_99`, `kAE1799AddedVFuncCount` or `AE_CHECK`. [V] Facts only; no code copied.

### What changed in 1.7.99, by category

| Category | Examples (NG 9955ae3 file:line) | Visible to the bridge? |
|---|---|---|
| Functions renumbered (old ID dropped, same function under a new ID) | `AE_CHECK` at `include/RE/C/CombatUtilities.h:15`, `src/RE/B/BSScaleformManager.cpp:21`, `src/RE/R/Renderer.cpp:112`, `src/RE/T/TES.cpp:209` | **yes**: ID level |
| Virtual functions inserted into interfaces | `MenuEventHandler.h:42-59` (+2 at slot 2), `PlayerInputHandler.h:39-52` (+2 at 2), `BSSystemUtility.h:53` (+1 at 2), `BSSaveDataSystemUtility.h:137` (+6 at 5) | **detectable** via the plugin's RTTI when the plugin derives from them |
| Members moved | `SkyrimVM.h:293-423` (+0x10 from 0x180; `impl` 0x200→0x210), `PlayerCharacter.h:1082-1102` (+8), `BSInputEventQueue.h`, `BookMenu.h`, `LockpickingMenu.h`, `bhkWorld.h`, `TESTopicInfo.h`, `CreateGrassParams.h`, `AttackBlockHandler.h`, `LookHandler.h`, `BGSDefaultObjectManager.cpp` | **no**: compiled into plugin code as constants |

### Implemented

1. **Known renumbered functions are translated.** The four `AE_CHECK` pairs are built in
   (`src/idtable.cpp`, `known_remaps`). Checked on Address Library v13: every old ID exists in
   1.6.1170 and is absent from 1.7.99/1.7.104; every new ID exists in both. Without this they were
   poisoned. Applied only when legacy is 1.6.x and the game is ≥ 1.7.99, and only if both IDs exist
   in the installed files. `Legacy\compat\_global.ids` adds rules for every plugin.
2. **Resolved offsets are checked against the running exe** (`src/verify.cpp`, `verify_ids=1`): every
   translated offset must be inside SkyrimSE.exe; offsets in code are compared with the exe's `.pdata`
   function starts. A broken translation would show offsets outside the image or a low share of
   function starts. `Legacy\compat\<plugin>.check` (any text; numbers are taken as IDs) gives a per-ID
   report: old offset, new offset, section, function-start yes/no, or MISSING.
3. **RTTI scan** (`src/rtti.cpp`, `rtti_scan=1`): reads the loaded legacy DLL's MSVC RTTI (vtable →
   CompleteObjectLocator → class hierarchy) and reports every plugin class whose bases include an
   interface from the vtable row above, with its vtable address and slot count. Read-only.
4. **Fault log** (`src/faultlog.cpp`, `fault_log=1`): a vectored exception handler that only logs and
   always continues the search. It records first-chance exceptions whose instruction is in a hosted
   DLL, whose stack contains a hosted DLL, or whose registers hold a poisoned address (decoded to the
   missing ID). SkyrimSE.exe frames are mapped back to the nearest Address Library ID. MSVC C++
   exceptions show their type and `what()`. First-chance matters: an exception the plugin or game
   catches leaves no crash, only a plugin that "stops working".
5. **Source checker** (`tools/check_plugin_source.py`): points at source lines that use the
   member-moved or vtable-changed APIs above, and at IDs on the bridge's missing-ID list.

### Not possible at the bridge level, and why

| Problem | Why it cannot be fixed generically | Safest practical alternative |
|---|---|---|
| Moved members (e.g. `SkyrimVM::impl`, `PlayerCharacter` runtime data) | The offset is an immediate in the plugin's machine code (`mov rax,[rcx+200h]`). Changing it means rewriting plugin code; changing game memory to match would corrupt the game. | Rebuild the plugin against a 1.7.99-aware CommonLib, or replace the affected call (e.g. use the `IVirtualMachine*` SKSE passes to the Papyrus callback). |
| Inserted virtual slots in an interface the **plugin implements** (`MenuEventHandler`, `PlayerInputHandler`) | The game calls slot *n+k* on the plugin's own vtable, which has the 1.6 layout. A fix needs a re-laid-out vtable and every object switched to it; object vptrs are written by the plugin's constructors. | Rebuild. Technically possible later as a targeted, opt-in adaptation for one named class (new vtable + vptr swap at a registration point), only once the RTTI report shows it is the cause. Not implemented. |
| Inserted virtual slots in a game interface the **plugin calls** | Slot index compiled into the call site. | Rebuild. |
| CommonLib-internal use of moved members (e.g. `PackUnpack.cpp` calling `VirtualMachine::GetSingleton()` while marshalling Papyrus arguments) | Same as moved members, inside statically linked CommonLib code. | Rebuild. The source checker flags `RegisterFunction`. |
| Same ID, changed behaviour or signature; hook sites at `ID + offset` | Address Library carries no signatures. | Check the plugin's hooks against 1.7.99; the `.check` report flags mid-function IDs. |
| Code in `DllMain` / static initializers | Runs before the import patch. | Unchanged from Stage 2. |

## Stage 5 — messaging fan-out (2026-11-12, from the in-game logs)

**Observed** [V, your logs]: `skse64.log` shows two `registering plugin listener for SKSE at 1 of 3` lines
(both hosted plugins, both under the bridge's handle 1) and one `sending message type N to plugin 1` per
message. PickPocketRedone-NG ran its data-loaded setup; RTL-SUPPORT-NG (`ApplyXml=true`) never loaded its
XML.

**Cause** [V, SKSE source]: `PluginManager::registerListener` in skse64 checks "is listener already
registered?" by plugin handle and does not add a second listener for the same handle and sender. Only the
first hosted plugin to register receives SKSE messages. This is the identity collapse listed as a known
side effect since Stage 1; with two hosted plugins it silently disables the second one's message handler.

**Fix** (`src/skseproxy.cpp`): hosted plugins get their own `QueryInterface`. For `kMessaging` it returns a
proxy whose `RegisterListener` records (sender, callback, plugin) and registers the bridge once per sender
with SKSE; the bridge's callback delivers each message to every hosted listener for that sender, in
registration order. `Dispatch` and `GetEventDispatcher` are forwarded. No SEH around callbacks, so a crash
stays a crash. Reproduced and fixed in the harness (mock SKSE that skips duplicate handles).

**Still shared, not fixed:**
- Outgoing messages carry the bridge's sender name; a plugin listening to `"RTL-SUPPORT-NG"` will not hear
  the hosted one. Could be routed internally later.
- Co-save: SKSE keeps one unique ID and one set of callbacks per handle. The proxy forwards unchanged and
  logs `[serialization] WARNING` when a second hosted plugin claims it. Multiplexing co-save records would
  change the save format and is not done. Practical rule: at most one co-save plugin per bridge.

## Stage 6 — patch before DllMain / static initializers (2026-11-12, CompassNavigationOverhaul)

**Observed** [V, your log]: CNO's section ends after `lineage: Case B`; there is no `T0 LoadLibraryW` line.
`logf` flushes every line, so the process ended inside `LoadLibraryW`: in the plugin's `DllMain` / CRT
static initializers, which ran before the bridge's import patch. Not CNO-specific: any CommonLib plugin
that resolves an ID or `REL::Module` during static initialization (namespace-scope `REL::Relocation`,
static objects whose constructors use IDs) is classified SE on 1.7.x and dies in `report_and_fail`.
Which of these CNO does was not determined; the fix covers the whole class.

**Mechanism** (`src/earlyload.cpp`): `LdrRegisterDllNotification` (ntdll, documented). Before each legacy
`LoadLibraryW` the bridge arms a one-shot match on the DLL name. In the LOADED notification it checks
whether the module's import table is already bound (IAT slot ≠ import-name-table slot) instead of
assuming the loader's ordering; if bound it installs the same per-module interposer and registers the
module with the messaging and fault-log tables, then returns. The ID table and interposer config are
now built before `LoadLibraryW`. Under the loader lock the callback only writes the module's own IAT
and log lines; it loads nothing.

Also: `GetFileVersionInfoA` / `VerQueryValueA` are interposed like the W versions (CNO imports both).

**Harness** (Wine 9): a mock plugin doing the CommonLib start-up sequence in a static initializer.
Previous build: dies with "failed to open address library file", log ends at `lineage` (same signature as
the in-game CNO log). New build: notification arrives with imports bound, static init sees 1.6.1170.0 and
the private table, `SKSEPlugin_Load` returns true. Whether real Windows delivers the notification after
import binding is checked at runtime on every load and logged. [Unverified on Windows until your log.]

## Stage 7 — hosted plugins talking to each other (2026-11-12, CNO + InfinityUI)

**Observed** [V, your logs]: all four plugins loaded and were patched before DllMain. Then:
- `CompassNavigationOverhaul.dll listens to "InfinityUI" (SKSE REFUSED the registration)`; CNO.log:
  "Failed to register messaging listener for InfinityUI", "Infinity UI installation not detected".
- InfinityUI.log: "Failed to dispatch message to all listeners" on every menu load (its API messages).
- During save load: `[fault]` AV reading 0x18 in `CompassNavigationOverhaul.dll+0x43663`, called from the
  game at ID 51668+0x12D / 51612+0x1795, under `PlayerCharacter::CenterOnCell` (ID 40437, per CommonLib
  539d4ce `src/RE/P/PlayerCharacter.cpp:253`). No RTTI vtable warning for CNO.

**Cause of the messaging failure** [V]: SKSE only knows the bridge, so a listener for a hosted plugin's
name is refused and a hosted plugin's broadcast has no listeners.

**Cause of the crash** [V, CNO source in `example-sources/`]: CNO creates its `Compass` object only when
InfinityUI sends `kPreReplaceInstance` for the HUD compass (`source/MessageListeners.cpp:72-78`,
`CNO::Compass::InitSingleton`). `HUDMarkerManager` keeps `Compass* compass = Compass::GetSingleton()`
(`include/HUDMarkerManager.h:53`), so without IUI's messages it is null. CNO replaces `Compass` vfunc 1
with `UpdateCompass` (`include/Hooks.h:166`), which calls `SetMarkersExtraInfo` ->
`compass->UnfocusMarker()` (`source/HUDMarkerManager.cpp:154`) -> `Invoke` on a null object, reading a
member at +0x18. That is the logged fault (`read address 0x18`; game frames ID 51668 = `Compass::Update`,
ID 51612 = `HUDMenu::ProcessMessage`, per CNO `include/Hooks.h:48,58`). IUI dispatches with
`receiver = nullptr` (`InfinityUI-main/include/FullAPI.h:12`) and CNO checks `sender == "InfinityUI"`
(`MessageListeners.cpp:50`); the routed message carries the declared name "InfinityUI", so both match.

**Remaining 1.7.x risk in CNO, not addressed by the bridge**: its call-site hooks use offsets checked on
1.6.x (`include/Hooks.h:72,101-102,112,126-128,140`: UpdateQuests+0x180, UpdateLocations+0x13C/+0x24B/
+0x473, HUDMenu::ProcessMessage+0x1735, Compass::Update+0xAE). If 1.7.x changed those functions, the
5-byte patches land on the wrong instructions. Checkable read-only against SkyrimSE.exe.

**Fix** (`src/skseproxy.cpp`, `src/main.cpp`):
- All legacy DLLs are inspected before any is loaded; each `SKSEPlugin_Version` name is declared.
- `RegisterListener` for a declared hosted name stays inside the bridge.
- A hosted plugin's `Dispatch` is delivered to those listeners with its own name as `sender`, honouring
  `receiver`; it is also forwarded to SKSE unless addressed to a hosted plugin.
- `GetPluginInfo(name)` answers for hosted plugins (`infoVersion 1`, name, version).
- Caller identification: return address, then the nearest hosted frame on the stack (tail calls), then
  the plugin whose `SKSEPlugin_Load` is running on this thread.

**Harness**: mock SKSE that refuses unknown senders; "ConsumerMod" listens to "ProviderMod" and calls
`GetPluginInfo`; "ProviderMod" dispatches type 5 on kDataLoaded. Previous build: refused, null, nothing
delivered (CNO's symptoms). New build: listening, found, consumer received type 5 from ProviderMod.
