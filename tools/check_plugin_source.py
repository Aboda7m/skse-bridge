"""Scan a legacy plugin's source for CommonLib APIs whose game layout changed in Skyrim 1.7.99.

Usage: python check_plugin_source.py <plugin source dir> [SKSE-Bridge-missing-ids.txt]

The bridge translates Address Library IDs, but it cannot change member offsets or vtable slot numbers
that CommonLib compiled into the plugin. This tool points at the source lines that depend on them.
Changes are taken from CommonLibSSE-NG 9955ae3 (first NG revision set with 1.7.99 support), which
marks each with RUNTIME_SSE_1_7_99 / kAE1799*. A hit means "check this", not "this is the bug".

With the missing-ID list the bridge writes, it also reports Address Library IDs used in the source
that do not exist in the installed 1.7.x library.
"""
import os
import re
import sys

# (pattern, what changed in 1.7.99, consequence for a 1.6.1170 build)
RULES = [
    (r'\bSkyrimVM\b|Internal::VirtualMachine::GetSingleton|\bVirtualMachine::GetSingleton\b',
     'SkyrimVM members from +0x180 moved by +0x10 (impl: 0x200 -> 0x210)',
     'legacy VirtualMachine::GetSingleton() returns a wrong pointer; Papyrus calls through it misbehave. '
     'Use the IVirtualMachine* SKSE passes to your Papyrus registration callback instead.'),
    (r'\bRegisterFunction\b',
     'legacy CommonLib converts native arguments/returns that are forms or objects through '
     'VirtualMachine::GetSingleton() (539d4ce src/RE/P/PackUnpack.cpp:17,29,57)',
     'Papyrus natives that take or return forms/objects can fail even though registration succeeds'),
    (r'GetPlayerRuntimeData|GetCrimeValue|GetRaceData|GetGameStatsData|GetQuestTargetsLock|GetQuestLog|'
     r'GetQuestTargets|GetInfoRuntimeData|GetPlayerFlags|As(BGSActorCell|BGSActorDeath|PositionPlayer)EventSource',
     'PlayerCharacter runtime data and event sources moved by +8 bytes',
     'reads/writes the wrong PlayerCharacter fields'),
    (r'\bMenuEventHandler\b',
     '2 virtual functions inserted at slot 2 (ProcessKinect..ProcessButton: 2-5 -> 4-7)',
     'a class deriving from it gets the wrong callbacks; the bridge log names such classes (RTTI: INCOMPATIBLE VTABLE)'),
    (r'\b(PlayerInputHandler|HeldStateHandler|AttackBlockHandler|LookHandler)\b',
     '2 virtual functions inserted at slot 2 in PlayerInputHandler; AttackBlockHandler/LookHandler data moved',
     'derived classes get the wrong callbacks; member access reads wrong fields'),
    (r'\bBSInputEventQueue\b',
     'queue head/tail and event buffers moved',
     'reads the wrong input events'),
    (r'\b(BookMenu|LockpickingMenu)\b', 'runtime data block added/moved', 'member access reads wrong fields'),
    (r'\b(bhkWorld|TESTopicInfo|CreateGrassParams|BGSDefaultObjectManager)\b', 'layout changed', 'member access reads wrong fields'),
    (r'\b(BSSystemUtility|BSSaveDataSystemUtility)\b', 'virtual functions inserted', 'virtual calls hit the wrong function'),
    (r'\bINPUT_EVENT_TYPE\b|\bkAmiibo\b', '1.7.99 can deliver a new input event type (amiibo)',
     'switches over event types may see an unknown value'),
]

ID_PATTERNS = [
    re.compile(r'RELOCATION_ID\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)'),        # (SE, AE): AE is the 2nd
    re.compile(r'REL::RelocationID\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)'),
    re.compile(r'REL::ID\s*[\(\{]\s*(\d+)\s*[\)\}]'),
    re.compile(r'REL::VariantID\s*\(\s*(\d+)\s*,\s*(\d+)\s*,'),
]
EXTS = ('.h', '.hpp', '.hh', '.cpp', '.cc', '.cxx', '.inl', '.ipp')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    missing = set()
    if len(sys.argv) > 2:
        missing = {int(x) for x in re.findall(r'^\d+', open(sys.argv[2]).read(), re.M)}
    compiled = [(re.compile(p), what, effect) for p, what, effect in RULES]
    hits, idhits = 0, 0
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in ('.git', 'build', 'lib', 'extern', 'vcpkg_installed', '.xmake', 'external')]
        for f in files:
            if not f.endswith(EXTS):
                continue
            path = os.path.join(d, f)
            try:
                lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
            except OSError:
                continue
            for n, line in enumerate(lines, 1):
                code = line.split('//', 1)[0]
                for rx, what, effect in compiled:
                    if rx.search(code):
                        hits += 1
                        print(f'{path}:{n}: {line.strip()[:120]}')
                        print(f'    1.7.99: {what}\n    effect: {effect}')
                if missing:
                    for rx in ID_PATTERNS:
                        for m in rx.finditer(code):
                            for g in m.groups():
                                if g and int(g) in missing:
                                    idhits += 1
                                    print(f'{path}:{n}: ID {g} is not in the installed 1.7.x Address Library (poisoned by the bridge)')
    print(f'\n{hits} layout/vtable hit(s)' + (f', {idhits} missing-ID hit(s)' if missing else ''))


if __name__ == '__main__':
    main()
