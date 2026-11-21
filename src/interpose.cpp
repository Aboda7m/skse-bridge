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
#include "trace.h"

#include <deque>

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
			const wchar_t*  spoofW{ L"" };  // stable storage in g_spoofStrings
			const char*     spoofA{ "" };
		};
		std::deque<std::pair<std::wstring, std::string>> g_spoofStrings;  // one per spoofed version, never freed

		SRWLOCK                g_lock = SRWLOCK_INIT;
		std::vector<ModuleCtx> g_modules;
		std::vector<const void*> g_exeBlocks;  // version-info buffers known to describe the game exe
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

		// ---- presented module path ----
		// A hosted DLL lives in Legacy\, but its files (ini, data folder) are installed by mod managers next to where
		// the DLL normally sits, Data\SKSE\Plugins\. Plugins that locate their files from their own module path
		// (GetModuleFileName) are shown that normal path. Functions the plugin may call with that path for the DLL
		// itself (version info, LoadLibrary, GetModuleHandle, CreateFile, GetFileAttributes) are mapped back to the
		// real file, so nothing it does with its own path breaks.
		std::wstring norm(std::wstring_view p)
		{
			std::wstring r(p);
			std::replace(r.begin(), r.end(), L'/', L'\\');
			return r;
		}

		// If `path` is the presented path of a hosted DLL, its real path; otherwise empty.
		std::wstring real_for(const wchar_t* path)
		{
			if (!path) {
				return {};
			}
			const std::wstring p = norm(path);
			std::wstring       out;
			AcquireSRWLockShared(&g_lock);
			for (const auto& m : g_modules) {
				if (!m.cfg.presentPath.empty() && iequals(p, m.cfg.presentPath)) {
					out = m.cfg.realPath;
					break;
				}
			}
			ReleaseSRWLockShared(&g_lock);
			return out;
		}

		std::string real_for_a(const char* path)
		{
			if (!path) {
				return {};
			}
			wchar_t w[MAX_PATH * 2]{};
			if (!MultiByteToWideChar(CP_ACP, 0, path, -1, w, static_cast<int>(std::size(w)))) {
				return {};
			}
			const std::wstring r = real_for(w);
			if (r.empty()) {
				return {};
			}
			char a[MAX_PATH * 4]{};
			WideCharToMultiByte(CP_ACP, 0, r.c_str(), -1, a, static_cast<int>(sizeof(a)), nullptr, nullptr);
			return a;
		}

		bool presented_for(HMODULE h, std::wstring& out)
		{
			if (!h) {
				return false;
			}
			const auto a = reinterpret_cast<std::uintptr_t>(h);
			bool       found = false;
			AcquireSRWLockShared(&g_lock);
			for (const auto& m : g_modules) {
				if (m.base == a && !m.cfg.presentPath.empty()) {
					out = m.cfg.presentPath;
					found = true;
					break;
				}
			}
			ReleaseSRWLockShared(&g_lock);
			return found;
		}

		DWORD WINAPI Hook_GetModuleFileNameW(HMODULE h, LPWSTR buf, DWORD n)
		{
			std::wstring p;
			if (!buf || !n || !presented_for(h, p)) {
				return ::GetModuleFileNameW(h, buf, n);
			}
			const DWORD len = static_cast<DWORD>(p.size());
			if (len < n) {
				std::wmemcpy(buf, p.c_str(), len + 1);
				SetLastError(ERROR_SUCCESS);
				return len;
			}
			std::wmemcpy(buf, p.c_str(), n - 1);
			buf[n - 1] = 0;
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return n;
		}

		DWORD WINAPI Hook_GetModuleFileNameA(HMODULE h, LPSTR buf, DWORD n)
		{
			std::wstring p;
			if (!buf || !n || !presented_for(h, p)) {
				return ::GetModuleFileNameA(h, buf, n);
			}
			char a[MAX_PATH * 4]{};
			WideCharToMultiByte(CP_ACP, 0, p.c_str(), -1, a, static_cast<int>(sizeof(a)), nullptr, nullptr);
			const DWORD len = static_cast<DWORD>(std::strlen(a));
			if (len < n) {
				std::memcpy(buf, a, len + 1);
				SetLastError(ERROR_SUCCESS);
				return len;
			}
			std::memcpy(buf, a, n - 1);
			buf[n - 1] = 0;
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return n;
		}

#define BRIDGE_MAPW(arg) const std::wstring mapped_ = real_for(arg); const wchar_t* const arg##_ = mapped_.empty() ? (arg) : mapped_.c_str()
#define BRIDGE_MAPA(arg) const std::string mapped_ = real_for_a(arg); const char* const arg##_ = mapped_.empty() ? (arg) : mapped_.c_str()

		DWORD WINAPI Hook_GetFileVersionInfoSizeW(LPCWSTR file, LPDWORD h) { BRIDGE_MAPW(file); return ::GetFileVersionInfoSizeW(file_, h); }
		DWORD WINAPI Hook_GetFileVersionInfoSizeA(LPCSTR file, LPDWORD h) { BRIDGE_MAPA(file); return ::GetFileVersionInfoSizeA(file_, h); }
		HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR file) { BRIDGE_MAPW(file); return ::LoadLibraryW(file_); }
		HMODULE WINAPI Hook_LoadLibraryA(LPCSTR file) { BRIDGE_MAPA(file); return ::LoadLibraryA(file_); }
		HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR file, HANDLE f, DWORD fl) { BRIDGE_MAPW(file); return ::LoadLibraryExW(file_, f, fl); }
		HMODULE WINAPI Hook_LoadLibraryExA(LPCSTR file, HANDLE f, DWORD fl) { BRIDGE_MAPA(file); return ::LoadLibraryExA(file_, f, fl); }
		HMODULE WINAPI Hook_GetModuleHandleW(LPCWSTR file) { BRIDGE_MAPW(file); return ::GetModuleHandleW(file_); }
		HMODULE WINAPI Hook_GetModuleHandleA(LPCSTR file) { BRIDGE_MAPA(file); return ::GetModuleHandleA(file_); }
		DWORD WINAPI Hook_GetFileAttributesW(LPCWSTR file) { BRIDGE_MAPW(file); return ::GetFileAttributesW(file_); }
		DWORD WINAPI Hook_GetFileAttributesA(LPCSTR file) { BRIDGE_MAPA(file); return ::GetFileAttributesA(file_); }
		HANDLE WINAPI Hook_CreateFileW(LPCWSTR file, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t)
		{
			BRIDGE_MAPW(file);
			return ::CreateFileW(file_, a, sh, sa, cd, fl, t);
		}
		HANDLE WINAPI Hook_CreateFileA(LPCSTR file, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t)
		{
			BRIDGE_MAPA(file);
			return ::CreateFileA(file_, a, sh, sa, cd, fl, t);
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
			BRIDGE_MAPW(file);
			const BOOL        ok = ::GetFileVersionInfoW(file_, handle, len, data);
			ModuleCtx         ctx;
			if (ok && data && find_ctx(caller, ctx) && ctx.cfg.spoofVersion && is_game_exe(file)) {
				mark_block(ctx, data, "GetFileVersionInfoW");
			}
			return ok;
		}

		BOOL WINAPI Hook_GetFileVersionInfoA(LPCSTR file, DWORD handle, DWORD len, LPVOID data)
		{
			const void* const caller = BRIDGE_RETURN_ADDRESS();
			BRIDGE_MAPA(file);
			const BOOL        ok = ::GetFileVersionInfoA(file_, handle, len, data);
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
				*buffer = const_cast<char*>(ctx.spoofA);
				*len = static_cast<UINT>(std::strlen(ctx.spoofA) + 1);
				logf("[interpose] %s: VerQueryValueA(\"%s\") -> \"%s\" (spoofed)", ctx.name.c_str(), sub, ctx.spoofA);
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
				*buffer = const_cast<wchar_t*>(ctx.spoofW);
				*len = static_cast<UINT>(std::wcslen(ctx.spoofW) + 1);
				logf("[interpose] %s: VerQueryValueW(\"%s\") -> \"%s\" (spoofed)", ctx.name.c_str(), narrow(key).c_str(),
					narrow(ctx.spoofW).c_str());
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

#if SKB_TRACE
		// TEST-ONLY (trace build): remember which handles are duplicates of a hosted plugin's private table, so the
		// MapViewOfFile hook below can tell the plugin mapping it apart from every other mapping in the process.
		SRWLOCK                                            g_traceHandleLock = SRWLOCK_INIT;
		std::vector<std::pair<HANDLE, std::string>>        g_traceHandles;

		void trace_note_handle(HANDLE h, const std::string& plugin)
		{
			AcquireSRWLockExclusive(&g_traceHandleLock);
			g_traceHandles.emplace_back(h, plugin);
			ReleaseSRWLockExclusive(&g_traceHandleLock);
		}

		bool trace_plugin_for_handle(HANDLE h, std::string& plugin)
		{
			bool found = false;
			AcquireSRWLockShared(&g_traceHandleLock);
			for (const auto& e : g_traceHandles) {
				if (e.first == h) {
					plugin = e.second;
					found = true;
				}
			}
			ReleaseSRWLockShared(&g_traceHandleLock);
			return found;
		}

		LPVOID WINAPI Hook_MapViewOfFile(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow, SIZE_T bytes)
		{
			LPVOID view = ::MapViewOfFile(mapping, access, offHigh, offLow, bytes);
			std::string plugin;
			if (view && trace::enabled() && trace_plugin_for_handle(mapping, plugin)) {
				trace::on_table_view(plugin.c_str(), view, bytes);
			}
			return view;
		}
#endif

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
						SKB_TRACE_DO(trace_note_handle(dup, ctx.name);
							trace::emit("table_open", ctx.name, trace::Fields().s("mapping", narrow(name)).u("bytes", ctx.cfg.mappingBytes)));
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

		// commonlib-shared (libxse) REL::IDDB: FMemoryMap::create_impl opens "COMMONLIB_IDDB_OFFSETS_<version>" before
		// creating it and, when it exists, uses its contents without unpacking (commonlib-shared include/REX/FMemoryMap.h:106).
		HANDLE WINAPI Hook_OpenFileMappingA(DWORD access, BOOL inherit, LPCSTR name)
		{
			const void* const caller = BRIDGE_RETURN_ADDRESS();
			ModuleCtx         ctx;
			if (name && std::string_view(name).starts_with("COMMONLIB_IDDB_OFFSETS_") && find_ctx(caller, ctx) && ctx.cfg.sharedMapping) {
				HANDLE dup = nullptr;
				if (DuplicateHandle(GetCurrentProcess(), ctx.cfg.sharedMapping, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
					logf("[interpose] %s: OpenFileMappingA(\"%s\") -> bridge table (%zu bytes)", ctx.name.c_str(), name, ctx.cfg.sharedBytes);
					return dup;
				}
			}
			return ::OpenFileMappingA(access, inherit, name);
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


		// Patch audit (patchaudit.cpp): sees every VirtualProtect the hosted plugin makes on SkyrimSE.exe and may refuse
		// one that would write over the wrong game code. Calls on any other memory pass straight through.
		BOOL WINAPI Hook_VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect, PDWORD old)
		{
			const void* const caller = BRIDGE_RETURN_ADDRESS();
			ModuleCtx         ctx;
			if (!(find_ctx(caller, ctx) && ctx.cfg.patchAudit)) {
				return ::VirtualProtect(addr, size, protect, old);
			}
			if (!patch_audit_protect(ctx.name, addr, size, protect)) {
				SetLastError(ERROR_ACCESS_DENIED);
				return FALSE;
			}
			const BOOL ok = ::VirtualProtect(addr, size, protect, old);
			patch_audit_after(addr, protect, ok);
			return ok;
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
			const std::wstring w = cfg.spoof.wstr(L'.');
			auto it = std::find_if(g_spoofStrings.begin(), g_spoofStrings.end(), [&](const auto& p) { return p.first == w; });
			if (it == g_spoofStrings.end()) {
				g_spoofStrings.emplace_back(w, cfg.spoof.str('.'));
				it = std::prev(g_spoofStrings.end());
			}
			ctx.spoofW = it->first.c_str();
			ctx.spoofA = it->second.c_str();
		}
		g_modules.push_back(ctx);
		ReleaseSRWLockExclusive(&g_lock);

		if (cfg.spoofVersion) {
			r.getFileVersionInfoW = patch_iat(legacy, "GetFileVersionInfoW", reinterpret_cast<void*>(&Hook_GetFileVersionInfoW));
			r.verQueryValueW = patch_iat(legacy, "VerQueryValueW", reinterpret_cast<void*>(&Hook_VerQueryValueW));
			r.getFileVersionInfoA = patch_iat(legacy, "GetFileVersionInfoA", reinterpret_cast<void*>(&Hook_GetFileVersionInfoA));
			r.verQueryValueA = patch_iat(legacy, "VerQueryValueA", reinterpret_cast<void*>(&Hook_VerQueryValueA));
		}
		if (cfg.idTable) {
			r.openFileMappingW = patch_iat(legacy, "OpenFileMappingW", reinterpret_cast<void*>(&Hook_OpenFileMappingW));
			r.createFileMappingW = patch_iat(legacy, "CreateFileMappingW", reinterpret_cast<void*>(&Hook_CreateFileMappingW));
#if SKB_TRACE
			if (trace::enabled()) {
				const int hooked = patch_iat(legacy, "MapViewOfFile", reinterpret_cast<void*>(&Hook_MapViewOfFile));
				trace::emit("hook", ctx.name, trace::Fields().s("api", "MapViewOfFile").u("sites", static_cast<std::uint64_t>(hooked)));
			}
#endif
		}
		if (cfg.sharedMapping) {
			r.openFileMappingA = patch_iat(legacy, "OpenFileMappingA", reinterpret_cast<void*>(&Hook_OpenFileMappingA));
		}
		if (!cfg.presentPath.empty()) {
			const std::pair<const char*, void*> pathHooks[] = {
				{ "GetModuleFileNameW", reinterpret_cast<void*>(&Hook_GetModuleFileNameW) },
				{ "GetModuleFileNameA", reinterpret_cast<void*>(&Hook_GetModuleFileNameA) },
				{ "GetFileVersionInfoSizeW", reinterpret_cast<void*>(&Hook_GetFileVersionInfoSizeW) },
				{ "GetFileVersionInfoSizeA", reinterpret_cast<void*>(&Hook_GetFileVersionInfoSizeA) },
				{ "LoadLibraryW", reinterpret_cast<void*>(&Hook_LoadLibraryW) },
				{ "LoadLibraryA", reinterpret_cast<void*>(&Hook_LoadLibraryA) },
				{ "LoadLibraryExW", reinterpret_cast<void*>(&Hook_LoadLibraryExW) },
				{ "LoadLibraryExA", reinterpret_cast<void*>(&Hook_LoadLibraryExA) },
				{ "GetModuleHandleW", reinterpret_cast<void*>(&Hook_GetModuleHandleW) },
				{ "GetModuleHandleA", reinterpret_cast<void*>(&Hook_GetModuleHandleA) },
				{ "GetFileAttributesW", reinterpret_cast<void*>(&Hook_GetFileAttributesW) },
				{ "GetFileAttributesA", reinterpret_cast<void*>(&Hook_GetFileAttributesA) },
				{ "CreateFileW", reinterpret_cast<void*>(&Hook_CreateFileW) },
				{ "CreateFileA", reinterpret_cast<void*>(&Hook_CreateFileA) },
			};
			for (const auto& [fn, hook] : pathHooks) {
				r.modulePath += patch_iat(legacy, fn, hook);
			}
			// The version-info hooks also map the path; install them even when the version spoof is off.
			if (!cfg.spoofVersion) {
				r.modulePath += patch_iat(legacy, "GetFileVersionInfoW", reinterpret_cast<void*>(&Hook_GetFileVersionInfoW));
				r.modulePath += patch_iat(legacy, "GetFileVersionInfoA", reinterpret_cast<void*>(&Hook_GetFileVersionInfoA));
			}
		}
		if (cfg.patchAudit) {
			r.virtualProtect = patch_iat(legacy, "VirtualProtect", reinterpret_cast<void*>(&Hook_VirtualProtect));
		}
		FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
		return r;
	}
}
