// Patch a legacy module before its DllMain and C++ static initializers run.
//
// Problem (observed in game, 2026-11-12, CompassNavigationOverhaul): SKSE-Bridge.log stopped after the
// inspection lines and before "T0 LoadLibraryW". The log is flushed per line, so the process ended inside
// LoadLibraryW, i.e. in the plugin's own startup code. A CommonLib plugin that touches REL::ID / REL::Module
// during static initialization (e.g. a namespace-scope REL::Relocation) does so before the bridge used to
// install its import patch: on 1.7.x it is classified as SE and dies in report_and_fail looking for the
// SE Address Library (CommonLibSSE-NG 539d4ce include/REL/Module.h:339-348, include/REL/ID.h:317-323).
//
// Mechanism: ntdll's LdrRegisterDllNotification (documented on MSDN, "LdrDllNotification callback").
// The LOADED notification arrives after the image is mapped. Whether the loader has already written the
// import table at that point is not documented, so it is checked, not assumed (imports_bound()). If bound,
// the same per-module interposer is installed from the callback, ahead of DllMain. If not, nothing is
// patched there and host_one patches after LoadLibraryW as before; the log says which happened.
//
// The callback runs under the loader lock: it only patches the module's own IAT (VirtualProtect + writes),
// registers the module with the bridge's tables and writes log lines. It never loads a library.
#include "common.h"

namespace bridge
{
	namespace
	{
		struct UnicodeString
		{
			USHORT Length;
			USHORT MaximumLength;
			PWSTR  Buffer;
		};

		struct LoadedData
		{
			ULONG                Flags;
			const UnicodeString* FullDllName;
			const UnicodeString* BaseDllName;
			PVOID                DllBase;
			ULONG                SizeOfImage;
		};

		constexpr ULONG kReasonLoaded = 1;

		using NotifyFn = VOID(CALLBACK*)(ULONG reason, const LoadedData* data, PVOID ctx);
		using RegisterFn = LONG(NTAPI*)(ULONG flags, NotifyFn fn, PVOID ctx, PVOID* cookie);

		struct Armed
		{
			bool            active{ false };
			std::wstring    path;  // full path passed to LoadLibraryW
			std::wstring    name;  // file name used in logs and module tables
			InterposeConfig cfg;
			bool            translate{ false };
			bool            faultLog{ false };
			EarlyResult     result;
		};

		SRWLOCK g_lock = SRWLOCK_INIT;
		Armed   g_armed;
		bool    g_registered = false;
		bool    g_available = false;

		std::wstring view(const UnicodeString* s)
		{
			return s && s->Buffer ? std::wstring(s->Buffer, s->Length / sizeof(wchar_t)) : std::wstring();
		}

		VOID CALLBACK on_notify(ULONG reason, const LoadedData* data, PVOID)
		{
			if (reason != kReasonLoaded || !data) {
				return;
			}
			AcquireSRWLockExclusive(&g_lock);
			// Base-name match while armed: the bridge's own LoadLibraryW is the only load it waits for, and the
			// loader may spell the directory differently (case, "data" vs "Data").
			if (!g_armed.active || g_armed.result.sawModule || !iequals(view(data->BaseDllName), base_name(g_armed.path))) {
				ReleaseSRWLockExclusive(&g_lock);
				return;
			}
			auto&         a = g_armed;
			const HMODULE mod = static_cast<HMODULE>(data->DllBase);
			a.result.sawModule = true;
			a.result.bound = imports_bound(mod);
			if (a.result.bound) {
				const std::string n = narrow(a.name);
				skse_proxy_add_module(mod, n);
				if (a.faultLog) {
					fault_log_add_module(mod, n);
				}
				if (a.translate) {
					a.result.r = install_interposer(mod, a.name, a.cfg);
				}
				a.result.patched = true;
			}
			const bool bound = a.result.bound;
			ReleaseSRWLockExclusive(&g_lock);
			logf("  early: loader notification for %s at %p, imports %s%s", narrow(view(data->FullDllName)).c_str(), data->DllBase,
				bound ? "bound" : "NOT yet bound", bound ? " -> patched before DllMain / static initializers" : " -> will patch after LoadLibraryW (late)");
		}
	}

	void early_arm(const std::wstring& path, const std::wstring& name, const InterposeConfig& cfg, bool translate, bool faultLog)
	{
		if (!g_registered) {
			g_registered = true;
			const auto reg = reinterpret_cast<RegisterFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification")));
			PVOID cookie = nullptr;
			g_available = reg && reg(0, &on_notify, nullptr, &cookie) >= 0;  // kept for the process lifetime
			logf("early: loader notifications %s", g_available ? "registered" : "UNAVAILABLE (legacy DllMain runs unpatched)");
		}
		wchar_t full[MAX_PATH * 2]{};
		const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
		AcquireSRWLockExclusive(&g_lock);
		g_armed = {};
		g_armed.active = g_available;
		g_armed.path = (n && n < std::size(full)) ? std::wstring(full, n) : path;
		g_armed.name = name;
		g_armed.cfg = cfg;
		g_armed.translate = translate;
		g_armed.faultLog = faultLog;
		g_armed.result.available = g_available;
		ReleaseSRWLockExclusive(&g_lock);
	}

	EarlyResult early_disarm()
	{
		AcquireSRWLockExclusive(&g_lock);
		g_armed.active = false;
		const EarlyResult r = g_armed.result;
		ReleaseSRWLockExclusive(&g_lock);
		return r;
	}
}
