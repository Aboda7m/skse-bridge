# LICENSES

No legal conclusions are drawn here; uncertain points are flagged.

## Host license
Not chosen yet. The host has **no GPL inputs**, so a permissive license is open to you.
(`xmake.lua` previously said `GPL-3.0`, inherited from the template; that line was removed.)

## Inputs to the host (`src/`)

| Source | License | Revision | How it is used |
|---|---|---|---|
| CommonLibSSE-NG (alandtse/CommonLibVR, branch `ng`), © 2018 Ryan-rsm-McKenzie | MIT | `539d4ce50969e8d23f4d507a7bc3dd955a4de93f` | Struct layouts of `SKSEInterface` and `PluginVersionData`, version packing, and the format 1/2 Address Library unpack algorithm were transcribed/re-implemented in `src/common.h` and `src/idtable.cpp`. MIT requires keeping the copyright and permission notice with substantial portions: include `lib/commonlibsse-ng/LICENSE` text when distributing. |
| CommonLibSSE-NG | GPL-3.0-or-later with modding/linking exceptions | `9955ae3b18dd0aedbebb58fa207a4b7180eb6d67` | **Read only**, to learn the Address Library format 5 file layout (field order, dense array, 0 = absent). No code was copied; the reader in `src/idtable.cpp` was written from that description. **Flag:** whether re-implementing a file format learned from GPL source raises any obligation is a question for you; file formats as such are generally treated as facts, but I am not drawing that conclusion for you. If you want zero exposure, confirm the layout from an independent source and I will cite that instead. |
| Windows SDK (kernel32, version, bcrypt, shell32, ole32) | Microsoft SDK terms | — | System libraries, linked. |

## Not used by the host
`lib/commonlibsse-ng` (MIT), `ClibUtil` (MIT), `xbyak` (BSD-style), `src/`, `view/` (PrismaUI template) remain in the tree but are not compiled or linked.

## Never included
Address Library data (`versionlib-*.bin`, `version-*.bin`) is not copied, embedded or redistributed. The host reads the user's installed files at runtime.

## Legacy plugins
Not modified, not linked, not relicensed. They are loaded as separate DLLs at runtime.

## `dist/SKSE-Bridge.dll` (convenience build)
Built with zig 0.17.1 (clang, `x86_64-windows-gnu`), statically linking LLVM libc++/libunwind/compiler-rt (Apache-2.0 with LLVM exception) and mingw-w64 runtime (mostly ZPL-2.1/public domain; per-file notices vary). **Flag:** check those notices before redistributing that binary; an MSVC build avoids the question.
