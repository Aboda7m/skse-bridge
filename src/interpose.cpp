// Per-module import-table interposer.
//
// Only the legacy module's own import table is patched. No game code is hooked, nothing is
// pattern-scanned, and other modules keep calling the real Win32 functions.
//
// Why these four functions (evidence: CommonLibSSE-NG 539d4ce):
//   GetFileVersionInfoW + VerQueryValueW : src/REL/Version.cpp:7-33 reads the exe's ProductVersion
//       string. Presenting the legacy runtime version fixes runtime classification
//       (include/REL/Module.h:339-348) and the Address Library file name (include/REL/ID.h:317-323).
//   OpenFileMappingW : src/REL/ID.cpp:20-23,109 tries the shared table "CommonLibSSEOffsets-v2-<ver>"
//       before unpacking. Answering with the host's private table makes the legacy plugin resolve
//       its IDs against the installed (new) Address Library.
//   CreateFileMappingW : src/REL/ID.cpp:55-61. Only reached if the open failed. It is refused for
//       that name so a legacy plugin can never silently fall back to old-runtime offsets.
//
// Timing limit: the patch is applied after LoadLibraryW returns, so anything the legacy DLL
// does with versions or IDs inside DllMain / static initializers has already run unpatched.
#include "common.h"

#if defined(_MSC_VER)
#	include <intrin.h>
#	define BRIDGE_RETURN_ADDRESS() _ReturnAddress()
#else
#	define BRIDGE_RETURN_ADDRESS() __builtin_return_address(0)
#endif

namespace bridge
{
	namespace
	{
		constexpr std::wstring_view kMapPrefix = L"CommonLibSSEOffsets-v2-";

		struct ModuleCtx
		{
			std::uintptr_t  base{ 0 };
			std::uintptr_t  end{ 0 };
			std::string     name;
			InterposeConfig cfg;
		};

		SRWLOCK                g_lock = SRWLOCK_INIT;
		std::vector<ModuleCtx> g_modules;
		std::vector<const void*> g_exeBlocks;  // version-info buffers known to describe the game exe
		wchar_t                g_spoofString[32]{};
		char                   g_spoofStringA[32]{};
		VS_FIXEDFILEINFO       g_spoofFixed{};

		bool find_ctx(const void* addr, ModuleCtx& out)
		{
			const auto a = reinterpret_cast<std::uintptr_t>(addr);
			bool       found = false;
			AcquireSRWLockShared(&g_lock);
			for (const auto& m : g_modules) {
				if (a >= m.base && a < m.end) {
					out = m;
					found = true;
					break;
				}
			}
			ReleaseSRWLockShared(&g_lock);
			return found;
		}

		bool is_game_exe(const wchar_t* file)
		{
			return file && iequals(base_name(file), base_name(exe_path()));
		}

		bool ends_with_i(std::wstring_view s, std::wstring_view tail)
		{
			return s.size() >= tail.size() && iequals(s.substr(s.size() - tail.size()), tail);
		}

		void mark_block(const ModuleCtx& ctx, LPVOID data, const char* fn)
		{
			{
				AcquireSRWLockExclusive(&g_lock);
				bool known = false;
				for (const void* b : g_exeBlocks) {
					known = known || b == data;
				}
				if (!known) {
					if (g_exeBlocks.size() >= 64) {
						g_exeBlocks.erase(g_exeBlocks.begin());
					}
					g_exeBlocks.push_back(data);
				}
				ReleaseSRWLockExclusive(&g_lock);
				logf("[interpose] %s: %s(game exe) -> block %p marked for version spoof", ctx.name.c_str(), fn, data);
			}
		}

		bool is_marked(LPCVOID block)
		{
			bool marked = false;
			AcquireSRWLockShared(&g_lock);
			for (const void* b : g_exeBlocks) {
				marked = marked || b == block;
			}
			ReleaseSRWLockShared(&g_lock);
			return marked;
		}

		// VS_FIXEDFILEINFO with the spoofed version, built from the real one. Returns null if unavailable.
		const VS_FIXEDFILEINFO* spoof_fixed(const void* real, UINT realLen, const Version& v)
		{
			if (!real || realLen < sizeof(VS_FIXEDFILEINFO)) {
				return nullptr;
			}
			AcquireSRWLockExclusive(&g_lock);
			g_spoofFixed = *static_cast<const VS_FIXEDFILEINFO*>(real);
			g_spoofFixed.dwFileVersionMS = g_spoofFixed.dwProductVersionMS = MAKELONG(v.v[1], v.v[0]);
			g_spoofFixed.dwFileVersionLS = g_spoofFixed.dwProductVersionLS = MAKELONG(v.v[3], v.v[2]);
			ReleaseSRWLockExclusive(&g_lock);
			return &g_spoofFixed;
		}

		BOOL WINAPI Hook_GetFileVersionInfoW(LPCWSTR file, DWORD handle, DWORD len, LPVOID data)
		{
			const void* const caller = BRIDGE_RETURN_ADDRESS();
			const BOOL        ok = ::GetFileVersionInfoW(file, handle, len, data);
			ModuleCtx         ctx;
			if (ok && data && find_ctx(caller, ctx) && ctx.cfg.spoofVersion && is_game_exe(file)) {
				mark_block(ctx, data, "GetFileVersionInfoW");
			}
			return ok;
		}

		BOOL WINAPI Hook_GetFileVersionInfoA(LPCSTR file, DWORD handle, DWORD len, LPVOID data)
		{
			const void* const caller = BRIDGE_RETURN_ADDRESS();
			const BOOL        ok = ::GetFileVersionInfoA(file, handle, len, data);
			ModuleCtx         ctx;
			if (ok && data && file && find_ctx(caller, ctx) && ctx.cfg.spoofVersion) {
				// ANSI path: convert with the ANSI code page, as the A API itself does.
				wchar_t    wide[MAX_PATH]{};
				const bool conv = MultiByteToWideChar(CP_ACP, 0, file, -1, wide, MAX_PATH) > 0;
				if (conv && is_game_exe(wide)) {
					mark_block(ctx, data, "GetFileVersionInfoA");
				}
			}
			return ok;
		}

		BOOL WINAPI Hook_VerQueryValueA(LPCVOID block, LPCSTR sub, LPVOID* buffer, PUINT len)
		{
			const void* const caller = BRIDGE_RETURN_ADDRESS();
			ModuleCtx         ctx;
			if (!(block && sub && buffer && len && find_ctx(caller, ctx) && ctx.cfg.spoofVersion && is_marked(block))) {
				return ::VerQueryValueA(block, sub, buffer, len);
			}
			const std::wstring key = widen(sub);
			if (ends_with_i(key, L"\\ProductVersion") || ends_with_i(key, L"\\FileVersion")) {
				*buffer = g_spoofStringA;
				*len = static_cast<UINT>(std::strlen(g_spoofStringA) + 1);
				logf("[interpose] %s: VerQueryValueA(\"%s\") -> \"%s\" (spoofed)", ctx.name.c_str(), sub, g_spoofStringA);
				return TRUE;
			}
			if (key == L"\\") {
				void* real = nullptr;
				UINT  realLen = 0;
				if (::VerQueryValueA(block, sub, &real, &realLen)) {
					if (const auto* f = spoof_fixed(real, realLen, ctx.cfg.spoof)) {
						*buffer = const_cast<VS_FIXEDFILEINFO*>(f);
						*len = sizeof(VS_FIXEDFILEINFO);
						logf("[interpose] %s: VerQueryValueA(\"\\\") -> VS_FIXEDFILEINFO %s (spoofed)", ctx.name.c_str(), ctx.cfg.spoof.str('.').c_str());
						return TRUE;
					}
				}
			}
			logf("[interpose] %s: VerQueryValueA(\"%s\") forwarded unchanged", ctx.name.c_str(), sub);
			return ::VerQueryValueA(block, sub, buffer, len);
		}

		BOOL WINAPI Hook_VerQueryValueW(LPCVOID block, LPCWSTR sub, LPVOID* buffer, PUINT len)
		{
			const void* const caller = BRIDGE_RETURN_ADDRESS();
			ModuleCtx         ctx;
			if (!(block && sub && buffer && len && find_ctx(caller, ctx) && ctx.cfg.spoofVersion && is_marked(block))) {
				return ::VerQueryValueW(block, sub, buffer, len);
			}
			const std::wstring_view key(sub);
			if (ends_with_i(key, L"\\ProductVersion") || ends_with_i(key, L"\\FileVersion")) {
				*buffer = g_spoofString;
				*len = static_cast<UINT>(std::wcslen(g_spoofString) + 1);
				logf("[interpose] %s: VerQueryValueW(\"%s\") -> \"%s\" (spoofed)", ctx.name.c_str(), narrow(key).c_str(),
					narrow(g_spoofString).c_str());
				return TRUE;
			}
			if (key == L"\\") {
				void*      real = nullptr;
				UINT       realLen = 0;
				const BOOL ok = ::VerQueryValueW(block, sub, &real, &realLen);
				const auto v = ctx.cfg.spoof;
				if (const auto* f = ok ? spoof_fixed(real, realLen, v) : nullptr) {
					*buffer = const_cast<VS_FIXEDFILEINFO*>(f);
					*len = sizeof(VS_FIXEDFILEINFO);
					logf("[interpose] %s: VerQueryValueW(\"\\\") -> VS_FIXEDFILEINFO %s (spoofed)", ctx.name.c_str(),
						v.str('.').c_str());
					return TRUE;
				}
			}
			logf("[interpose] %s: VerQueryValueW(\"%s\") forwarded unchanged", ctx.name.c_str(), narrow(key).c_str());
			return ::VerQueryValueW(block, sub, buffer, len);
		}

		HANDLE WINAPI Hook_OpenFileMappingW(DWORD access, BOOL inherit, LPCWSTR name)
		{
			const void* const caller = BRIDGE_RETURN_ADDRESS();
			ModuleCtx         ctx;
			if (name && std::wstring_view(name).starts_with(kMapPrefix) && find_ctx(caller, ctx)) {
				if (ctx.cfg.mapping) {
					HANDLE dup = nullptr;
					if (DuplicateHandle(GetCurrentProcess(), ctx.cfg.mapping, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
						logf("[interpose] %s: OpenFileMappingW(\"%s\") -> private table (%zu bytes)", ctx.name.c_str(),
							narrow(name).c_str(), ctx.cfg.mappingBytes);
						return dup;
					}
					logf("[interpose] %s: OpenFileMappingW(\"%s\"): DuplicateHandle failed (%lu)", ctx.name.c_str(),
						narrow(name).c_str(), static_cast<unsigned long>(GetLastError()));
				} else {
					logf("[interpose] %s: OpenFileMappingW(\"%s\"): no private table; refusing", ctx.name.c_str(), narrow(name).c_str());
				}
				SetLastError(ERROR_FILE_NOT_FOUND);
				return nullptr;
			}
			return ::OpenFileMappingW(access, inherit, name);
		}

		HANDLE WINAPI Hook_CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD hi, DWORD lo, LPCWSTR name)
		{
			const void* const caller = BRIDGE_RETURN_ADDRESS();
			ModuleCtx         ctx;
			if (name && std::wstring_view(name).starts_with(kMapPrefix) && find_ctx(caller, ctx)) {
				logf("[interpose] %s: CreateFileMappingW(\"%s\") REFUSED: the legacy plugin tried to build its own ID table from the old Address Library",
					ctx.name.c_str(), narrow(name).c_str());
				SetLastError(ERROR_ACCESS_DENIED);
				return nullptr;
			}
			return ::CreateFileMappingW(file, sa, protect, hi, lo, name);
		}

		// Replace every import-table slot of `mod` that imports `fn` by name. Returns slots patched.
		int patch_iat(HMODULE mod, const char* fn, void* hook)
		{
			auto* const base = reinterpret_cast<std::uint8_t*>(mod);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (!dir.VirtualAddress) {
				return 0;
			}
			int patched = 0;
			for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name || desc->FirstThunk; ++desc) {
				if (!desc->OriginalFirstThunk || !desc->FirstThunk) {
					continue;  // no name table to match against
				}
				const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);
				auto*       slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
				for (; names->u1.AddressOfData; ++names, ++slots) {
					if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
						continue;
					}
					const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
					if (std::strcmp(reinterpret_cast<const char*>(byName->Name), fn) != 0) {
						continue;
					}
					DWORD old = 0;
					if (VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
						slots->u1.Function = reinterpret_cast<ULONGLONG>(hook);
						VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
						++patched;
					}
				}
			}
			return patched;
		}
	}

	// True once the loader has written resolved addresses into the module's import table. Before binding,
	// each IAT slot still holds the same value as its import-name-table slot (an RVA).
	bool imports_bound(HMODULE mod)
	{
		auto* const base = reinterpret_cast<const std::uint8_t*>(mod);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress) {
			return true;  // nothing to bind
		}
		for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name || desc->FirstThunk; ++desc) {
			if (!desc->OriginalFirstThunk || !desc->FirstThunk) {
				continue;
			}
			const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);
			const auto* slots = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
			if (names->u1.AddressOfData) {
				return slots->u1.Function != names->u1.AddressOfData;
			}
		}
		return true;
	}

	InterposeResult install_interposer(HMODULE legacy, const std::wstring& name, const InterposeConfig& cfg)
	{
		InterposeResult r;
		auto* const     base = reinterpret_cast<std::uint8_t*>(legacy);
		const auto*     dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto*     nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);

		ModuleCtx ctx;
		ctx.base = reinterpret_cast<std::uintptr_t>(base);
		ctx.end = ctx.base + nt->OptionalHeader.SizeOfImage;
		ctx.name = narrow(name);
		ctx.cfg = cfg;

		AcquireSRWLockExclusive(&g_lock);
		if (cfg.spoofVersion) {
			const auto s = cfg.spoof.wstr(L'.');
			std::wmemset(g_spoofString, 0, std::size(g_spoofString));
			std::wmemcpy(g_spoofString, s.c_str(), std::min<std::size_t>(s.size(), std::size(g_spoofString) - 1));
			const auto a = cfg.spoof.str('.');
			std::memset(g_spoofStringA, 0, sizeof(g_spoofStringA));
			std::memcpy(g_spoofStringA, a.c_str(), std::min<std::size_t>(a.size(), sizeof(g_spoofStringA) - 1));
		}
		g_modules.push_back(ctx);
		ReleaseSRWLockExclusive(&g_lock);

		if (cfg.spoofVersion) {
			r.getFileVersionInfoW = patch_iat(legacy, "GetFileVersionInfoW", reinterpret_cast<void*>(&Hook_GetFileVersionInfoW));
			r.verQueryValueW = patch_iat(legacy, "VerQueryValueW", reinterpret_cast<void*>(&Hook_VerQueryValueW));
			r.getFileVersionInfoA = patch_iat(legacy, "GetFileVersionInfoA", reinterpret_cast<void*>(&Hook_GetFileVersionInfoA));
			r.verQueryValueA = patch_iat(legacy, "VerQueryValueA", reinterpret_cast<void*>(&Hook_VerQueryValueA));
		}
		r.openFileMappingW = patch_iat(legacy, "OpenFileMappingW", reinterpret_cast<void*>(&Hook_OpenFileMappingW));
		r.createFileMappingW = patch_iat(legacy, "CreateFileMappingW", reinterpret_cast<void*>(&Hook_CreateFileMappingW));
		FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
		return r;
	}
}
