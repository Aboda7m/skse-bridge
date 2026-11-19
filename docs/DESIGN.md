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


## Stage 8 — patch audit and patch guard (2026-11-19, Improved Camera SE 1.1.2)

**Symptom** (user's logs, 1.7.104): ImprovedCameraSE.dll loads through the bridge (early patch, spoofed
1.6.1170.0, private ID table, `T1 ... true`, messages 0/1/6/8/2 delivered, RTTI 0 incompatible), reaches the
main menu, loads a save and the game crashes. No `[fault]` line, no crash-logger file in the folder.

**Evidence, source** (ArranzCNL/ImprovedCameraSE-NG 2e441c1, source/skyrimse/Hooks.cpp:543-633) [Verified]:
all hooks are installed in `SKSEPlugin_Load`. Most are `ID + offset` call-site hooks (`write_thunk_call<5/6>`),
byte patches (`REL::safe_write`), two function-start MinHook hooks, two whole-function `asm_replace`, and two
`write_vfunc` (BSLookAtModifier slot 23, **TogglePOVHandler slot 4**). The offsets are hand-picked per runtime
(`REL::VariantOffset(SE, AE, VR)`); the AE value was read off 1.6.x code.

**Evidence, Address Library** (1.6.1170 vs 1.7.104 files, read-only; "span" = distance to the next ID) [Verified]:
every IC ID exists in 1.7.104. Spans are unchanged for all sites except three:

| IC site | AE ID + offset | span 1.6.1170 → 1.7.104 | when it runs |
|---|---|---|---|
| TESObjectCell_Get3D (`write_call<6>`) | 19165 + 0x7B | 0xA30 → 0xA90 | cell 3D load (save load) |
| SmoothAnimationTransitions (`write_call<6>`) | 41996 + 0x2F4 | 0x900 → 0x920 | actor animation update |
| HorseLookingDownFix1 (byte patch) | 37356 + 0xB64 | 0xFA0 → 0xFB0 | mounted camera |

A changed span means that function was recompiled. CommonLib-NG's `write_call<6>` assumes `FF 15 disp32`
at the site, takes the original target from that displacement and overwrites 6 bytes. If 1.7.104 has a
different instruction at +0x7B, the patch cuts an instruction in half and the saved "original" pointer is
garbage; the crash then happens later, in game code, with no IC frame (matches: no `[fault]`, crash on save
load). [Assumption] until the bytes at those sites are read: the 0.4.0 log prints them.

Also found [Verified]: `TogglePOVHandler` derives from `PlayerInputHandler`, which gained 2 slots at index 2
in 1.7.99, so IC's slot-4 hook (1.6 `ProcessButton`) now replaces `ProcessThumbstick` (wrong feature, not a
crash: the thunk calls the saved original). IC also writes `PlayerCharacter::playerFlags` (moved +8) around
its ShaderReferenceEffect hooks (wrong field toggled and restored). These cannot be fixed by the bridge.

**Fix — universal, not IC-specific** (`src/patchaudit.cpp`, VirtualProtect slot in `src/interpose.cpp`):
- The hosted plugin's import of `VirtualProtect` is interposed (its own IAT only). CommonLib-NG
  `safe_write`/`safe_fill`, the trampoline and MinHook all unlock the target with VirtualProtect before
  writing and restore after (NG `src/REL/Relocation.cpp:7-30`) [Verified].
- Every unlock of SkyrimSE.exe memory is judged against both Address Libraries (IDs present in both):
  function start → ok; span unchanged → ok; span changed and the 5/6-byte write sits on `E8/E9` /
  `FF 15/FF 25` → allowed with a warning; span changed otherwise → **unsafe**. Vtable writes are located
  from the exe's own RTTI; a slot at/after an inserted index (rtti.cpp table) → **unsafe**.
- `patch_guard=1` (default) makes the unlock of an unsafe write fail. NG's `safe_write` then skips the copy
  (the assert is a no-op in release) [Verified in source]; the feature is missing, game code stays intact.
  MinHook reports `MH_ERROR_MEMORY_PROTECT`. Other libraries may react differently [Unknown].
- `[patch]` lines: plugin, `SkyrimSE.exe+RVA (ID n+0xD)`, bytes before → after, verdict. Summary after each
  `SKSEPlugin_Load` and after kDataLoaded.

**Limits**: equal span is evidence, not proof, of unchanged code; a changed function that keeps the same
size passes. Plugins that write through other APIs (WriteProcessMemory, direct writes after their own
VirtualProtect on a larger region, `GetProcAddress("VirtualProtect")`) are seen only partly or not at all.
Blocking removes features; it does not make the plugin correct. Rebuilding for 1.7 remains the real fix.

**Harness** (Wine, synthetic exe with RTTI for `TogglePOVHandler : PlayerInputHandler`, synthetic ALs):
5-byte call over E8 in unchanged function → ok; over E8 in changed function → warned; over `48 89 C8` in
changed function → BLOCKED; 14-byte write at function start → ok; 2-byte payload in changed function →
BLOCKED; vtable slot 1 → ok; vtable slot 4 → BLOCKED ("meant the function that is now slot 6"). Old
harness (OldPlugin) unchanged. Builds; needs in-game verification.


## Stage 9 — 0.5.0: co-save sub-spaces, presented module path, sharper patch guard (2026-11-19)

### IC sites checked against the real 1.7.104 SkyrimSE.exe [Verified]
`example-sources/skyrim/SkyrimSE.exe` (1.7.104, read-only, disassembled with capstone at AL 1.7.104 offsets):

| site | 1.7.104 instruction at the offset | 0.4.0 verdict | correct? |
|---|---|---|---|
| TESObjectCell_Get3D 19165+0x7B (6-byte) | **inside** `call [rax+0x468]`, which starts at +0x7A | blocked | yes, would cut an instruction |
| SmoothAnimationTransitions 41996+0x2F4 (6-byte) | **inside** `je`, which starts at +0x2F3 | blocked | yes |
| HorseLookingDownFix1 37356+0xB64 (2-byte `66 90`) | `ja short` (`77 10`), instruction start | blocked | no: false positive |
| TogglePOVHandler vtable slot 4 | slot moved (PlayerInputHandler +2@2) | blocked | yes |
| all other IC sites | calls / jcc / movss at instruction starts | ok | yes |

So the save-load crash in 0.3.0 was the TESObjectCell or SmoothAnimation hook writing into the middle of an
instruction. 0.5.0 recognises the instruction shapes byte patches usually replace (2-byte short jump,
6-byte `0F 8x` jcc, 6-byte `FF 9x` virtual call) so HorseLookingDownFix1 is now allowed (with a warning).
Remapping a misplaced hook to the right offset (+0x7A) is not done: it would mean rewriting the plugin's
write and its saved original-call pointer, which is the binary rewriting this project excludes.

### Co-save sub-spaces (`src/skseproxy.cpp`, `cosave_multiplex=1`)
SKSE keeps one unique ID and one callback set per plugin handle; all hosted plugins share the bridge's handle.
The bridge now registers itself once as `'SKBR'` and keeps one slot per hosted plugin (caller attribution as
for messaging). Its save callback calls each hosted save callback with a proxy interface:
`WriteRecord(type, ver, data)` and `OpenRecord`+`WriteRecordData` become one SKSE record
`(type = hosted unique ID, version = ver, data = [type][data])`. Its load callback reads every record,
groups them by hosted unique ID and calls each hosted load callback with a proxy whose
`GetNextRecordInfo`/`ReadRecordData` serve only that plugin's records; `ResolveFormId`/`ResolveHandle` are
forwarded (valid while the bridge's load callback runs). Revert and form-delete fan out. Records whose
unique ID no hosted plugin claims are kept and written back unchanged. Layout: NG 539d4ce
`SKSESerializationInterface` [Verified]. SKSE's own co-save keying by unique ID [Assumption: from SKSE's
documented API, not its 1.7.104 binary].
Migration: data a hosted plugin saved before 0.5.0 sits under its own ID in SKSE's file; SKSE only hands the
bridge the `'SKBR'` chunk, so that data is not read [Assumption, same source].

### Presented module path (`src/interpose.cpp`, `present_plugin_path=1`)
Hosted DLLs live in `Legacy\`; their ini/data files are installed next to `Data\SKSE\Plugins\`. The hosted
module's `GetModuleFileNameW/A` for its own handle returns `Data\SKSE\Plugins\<file>`. Calls it may make with
that path for its own DLL (`GetFileVersionInfo[Size]W/A`, `LoadLibrary[Ex]W/A`, `GetModuleHandleW/A`,
`CreateFileW/A`, `GetFileAttributesW/A`) are mapped back to the real file. Limit: file access through the
dynamic CRT (fopen, std::ifstream, std::filesystem) runs in ucrtbase.dll, not in the plugin's import table,
and is not mapped; it only matters for the plugin's own DLL path, which plugins rarely open that way.

### Harness (Wine)
Two co-save plugins with different IDs, both `WriteRecord` and `OpenRecord`/`WriteRecordData`: mock SKSE sees
one chunk `'SKBR'` with 4 records; after revert+load each plugin reads exactly its own 2 records (values,
lengths, ResolveFormId forwarded); second save round-trips. Presented path: `GetModuleFileNameW` →
`Data\SKSE\Plugins\X.dll`; `LoadLibraryW` of that path returns the same module; `GetFileAttributesW` finds it.
Patch guard: 2-byte write over `ja short` in a changed function → warned and written; over `CC` → blocked;
allow-listed mid-instruction 5-byte write → written. Old harness unchanged. Builds; needs in-game verification.


### 0.5.0 hotfix — SKSE rejected the bridge itself
Symptom: with the 0.5.0 build SKSE disabled SKSE-Bridge.dll ("must be recompiled for new address library");
0.4.0 loaded. Cause [Verified, skse64_1_7_104.dll 2.3.1 disassembly, RVA 0x810A9-0x810DE]: for a plugin that
declares `AddressLibraryPostAE` but not `versionIndependenceEx` bit 1, SKSE reads the DLL's PE TimeDateStamp and
rejects it if that lies in 0x1F008600..0x68338EFF (1986-06-26 .. 2025-05-25 UTC). lld and MSVC `/Brepro` write a
content hash there, so whether a build passed was luck (the 0.5.0 hash decoded to 2008). Fix: the host declares
Ex bit 1 (`kVersionIndependentEx_AddressLibraryV5`, our name); it does read the 1.7.x format-5 library itself.
Hosted legacy DLLs are never checked by SKSE, so this does not apply to them.


### 1.6.1170 binaries (example-sources/1.6.1170) — checks [Verified, read-only disassembly]
- **SKSE 2.2.6 (`skse64_1_6_1170.dll`) accepts SKSE-Bridge.** Its compatibility check (RVA 0x81FE0-0x82124):
  rejects only `versionIndependence & ~7`, missing Address Library file (call 0x819B0), AL/Signatures without
  StructsPost629 or Ex NoStructUse, no listed runtime when not version independent, `xseMinimum > 2.2.6`.
  It has no TimeDateStamp test and ignores Ex bit 1. The bridge (Ind 0x5, Ex 0x3, xseMinimum 0) passes both
  2.2.6 and 2.3.1, so one DLL can serve both runtimes.
- **Improved Camera sites, 1.6.1170 vs 1.7.104:**

  | site | 1.6.1170 at IC's offset | same instruction in 1.7.104 |
  |---|---|---|
  | TESObjectCell_Get3D 19165+0x7B | `call [rax+0x468]` (FF 90), exact | at +0x7A (moved -1) |
  | SmoothAnimationTransitions 41996+0x2F4 | `call [rax+0x388]` (FF 90), exact | at +0x303 (moved +0xF) |
  | HorseLookingDownFix1 37356+0xB64 | `ja short` (77 10) | same offset |

  Confirms the guard's blocks and the horse false positive. Relocating hooks to the new offsets is not done
  (instruction matching = signature scanning, out of scope); a rebuilt IC would use +0x7A / +0x303.


### 1.6.640 inputs (example-sources/1.6.640) — findings for 0.6.0 [2026-11-19]
- **Address Library**: `versionlib-1-6-640-0.bin` ships in the user's AIO package (format 2, 416,334 IDs, max 434,106)
  [Verified]. The bridge can read it; nothing needs to be shipped.
- **Numbering vs 1.6.1170** [Verified, AL only]: 361,819 IDs in both; on those, 99.1% of spans are identical and
  offsets keep their order (1,014 inversions), so the numbering is the same scheme. 54,515 IDs exist only in 640;
  66,642 only in 1170 (all >= 400,000).
- **Renumbered functions 640 -> 1170** [Assumption, measured]: predicting an ID's 1170 offset from the nearest
  common ID below it ("anchor") is exact for 98.65% of 20,000 held-out common IDs. Applied to 640-only IDs:
  17,204 land exactly on a 1170-only ID (renumbered), 1,213 on a common ID (ambiguous, not used), 36,098 on
  nothing (gone). Checked against the three 1130 renumberings CommonLib-NG names (`AE_CHECK(1130, ...)`): 2 agree
  (21890->441582, 69188->443410), 0 disagree, 1 not found (405935->504099, data). Such a table must be computed at
  runtime from the user's installed files, never shipped (rule 3).
- **Layouts 640 -> 1130+** [Verified, NG 539d4ce source]: `ControlMap` runtime data 0xE8 -> 0xF0,
  `Renderer` depth-stencil/RendererData2/lock +0x60, `TES` runtime data replaced (0x128 -> AE block at 0x120,
  RUNTIME_DATA2 0x140 -> 0x148), `BGSSaveLoadManager` runtime data 0x2B0 -> 0x2F8. A 640-built plugin that touches
  these reads wrong fields on 1170 and on 1.7.x (where the 1.7.99 changes come on top).
- **SKSE 2.2.3 (`src/skse64/PluginManager.cpp`)** accepts the bridge (same rules as 2.2.6; no timestamp test)
  [Verified, source]. This also confirms the reading of the 2.2.6 disassembly.
- **1.6.640 SkyrimSE.exe is SteamStub-encrypted** (`.bind` section, `.text` entropy 8.00) [Verified]: no static
  code comparison is possible from it; it is not unpacked (DRM). The GOG build 1.6.659 is the same game
  generation without DRM, and the AIO package already has `versionlib-1-6-659-0.bin`.


## Stage 10 — 0.6.0: one DLL, every supported runtime, per-plugin target (2026-11-19)

**Per plugin** (`plan_for` in `src/main.cpp`), the runtime it was built for (target) is taken from, in order:
1. `[Targets] <file>=<version>|native` in LegacyHost.ini;
2. the version list in its `SKSEPlugin_Version` (non-independent plugins): the game itself if listed (native),
   else the first entry;
3. legacy CommonLibSSE-NG lineage (imports GetFileVersionInfoW/VerQueryValueW/OpenFileMappingW, not
   commonlib-shared): `legacy_runtime` (default 1.6.1170) — this lineage reads its runtime from the exe, so the
   DLL itself does not say which it was built against;
4. anything else: native (it loads the library for the running game itself; commonlib-shared also validates
   the file, so it must never be handed another one).
Target == game → native: no spoof, no ID table, interposer only for module path and patch audit.
Target != game and translatable → spoof the target, table = target library rows with offsets from the game's
library. Works in any direction between 1.6.x and 1.7.x because the numbering is shared (1.6.640 vs 1.6.1170:
99.1% identical spans; 1.6.1170 vs 1.7.x: 97.6%).

**Address Libraries** are loaded per version on demand (`lib_for`), from the user's installation only.
`SKSE-Bridge-missing-ids-<target>.txt` per target.

**Renumbered functions** (`known_remaps`): verified pairs per game update, applied forward (plugin before the
update, game after) or inverted (the reverse): 1.6.1130 (NG 539d4ce AE_CHECK: 21890->441582, 69188->443410,
405935->504099) and 1.7.99 (NG 9955ae3: 4 pairs). Inference from the two libraries (`infer_remaps`) exists but
is **off by default**: it reproduces all 4 verified 1.7.99 pairs, but against both real executables
(1.6.1170 and 1.7.104, unencrypted) only 64.6% of 4,176 code-checkable inferred pairs begin with the same code
(unchanged IDs: 99.1%; random pairs: 1.4%) and 14.5% look unrelated. A wrong remap calls the wrong function
silently; a missing ID faults at a named address. [Measured]

**Direction-aware diagnostics**: vtable checks (`vtable_direction`) apply when plugin and game lie on opposite
sides of 1.7.99, mirrored for the reverse (plugin built for 1.7 on 1.6). Patch audit compares each module
against its own target library. 1.6.1130 layout changes (ControlMap, Renderer, TES, BGSSaveLoadManager) are
documented (Stage 9 notes) but not detected automatically.

**Version spoof is per module** (stable strings per version), so 1.6.640 and 1.6.1170 plugins can be spoofed
differently in the same session.

**SKSE acceptance** of the one DLL [Verified]: 2.2.3 (source), 2.2.6 and 2.3.1 (disassembly) with
Ind 0x5, Ex 0x3, xseMinimum 0.

**Harness** (Wine): three mock games (1.7.99, 1.6.1170, 1.6.640, version resource + synthetic Address
Libraries in the real formats) × three plugins (default NG, `[Targets]=1.6.640`, declares 1.6.640):

| game | default NG (1.6.1170) | [Targets] 1.6.640 | declares 1.6.640 |
|---|---|---|---|
| 1.7.99 | translated | translated, 69188 -> 443410 (known 1130) | translated |
| 1.6.1170 | native | translated 640 -> 1170 | translated |
| 1.6.640 | translated 1170 -> 640, 443410 -> 69188 (inverted), 1.7-only ID poisoned | native | native |

0.5.0 harnesses (patch guard, co-save, module path, OldPlugin) unchanged. Builds; needs in-game verification.
