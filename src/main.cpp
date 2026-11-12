// SKSE-Bridge: legacy plugin host.
//
// Loads SKSE plugins built for an older runtime (default 1.6.1170) on a newer one (1.7.99+),
// without modifying them. What it adapts: loading, the runtime version the legacy plugin sees,
// and Address Library ID lookups. What it cannot adapt: struct layouts, vtable slots, or any
// change in what the game code behind an ID does.
#include "common.h"

#include <algorithm>

namespace bridge
{
	namespace
	{
		constexpr Version kHostVersion{ { 0, 1, 0, 0 } };

		struct Config
		{
			bool                      dryRun{ false };
			std::wstring              legacyDir;
			std::vector<std::wstring> plugins;  // empty = every *.dll in legacyDir
			bool                      translate{ true };
			bool                      spoofSkseRuntime{ true };
			bool                      idTable{ true };
			bool                      force{ false };
			Version                   spoof{ { 1, 6, 1170, 0 } };
			std::wstring              addressLibrary;  // empty = auto
			bool                      verifyIds{ true };
			bool                      rttiScan{ true };
			bool                      faultLog{ true };
			bool                      messagingFanout{ true };
		};

		struct Preflight
		{
			bool                 ok{ false };
			std::string          why;
			Version              real{};
			std::vector<IdEntry> legacy;   // from the legacy (1.6.1170) Address Library, sorted
			std::vector<IdEntry> entries;  // from the installed (real) Address Library, sorted
			RemapRules           baseRules;  // known renumbered functions + compat\_global.ids, applied to every plugin
		};

		std::wstring ini_string(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, const wchar_t* def)
		{
			wchar_t buf[2048]{};
			GetPrivateProfileStringW(sec, key, def, buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
			std::wstring s(buf);
			while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) {
				s.pop_back();
			}
			return s;
		}

		bool ini_bool(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, bool def)
		{
			return GetPrivateProfileIntW(sec, key, def ? 1 : 0, ini.c_str()) != 0;
		}

		std::wstring absolute(const std::wstring& p)
		{
			const bool isAbs = p.size() > 1 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\'));
			std::wstring r = isAbs ? p : game_dir() + p;
			std::replace(r.begin(), r.end(), L'/', L'\\');
			if (!r.empty() && r.back() != L'\\') {
				r += L'\\';
			}
			return r;
		}

		Config read_config()
		{
			Config             c;
			const std::wstring ini = game_dir() + L"Data\\SKSE\\Plugins\\LegacyHost.ini";
			logf("config: %s (%s)", narrow(ini).c_str(), file_exists(ini) ? "found" : "not found, using defaults");
			c.dryRun = ini_bool(ini, L"General", L"dry_run", false);
			c.legacyDir = absolute(ini_string(ini, L"General", L"legacy_dir", L"Data\\SKSE\\Plugins\\Legacy"));
			const auto list = ini_string(ini, L"General", L"plugins", L"*");
			std::size_t pos = 0;
			while (pos <= list.size()) {
				const auto   sep = list.find_first_of(L",;", pos);
				std::wstring item = list.substr(pos, sep == std::wstring::npos ? std::wstring::npos : sep - pos);
				pos = sep == std::wstring::npos ? list.size() + 1 : sep + 1;
				while (!item.empty() && item.front() == L' ') {
					item.erase(item.begin());
				}
				while (!item.empty() && item.back() == L' ') {
					item.pop_back();
				}
				if (!item.empty() && item != L"*") {
					c.plugins.push_back(base_name(item));  // names only; no paths outside legacy_dir
				}
			}
			c.translate = ini_bool(ini, L"Translate", L"enabled", true);
			c.spoofSkseRuntime = ini_bool(ini, L"Translate", L"spoof_skse_runtime", true);
			c.idTable = ini_bool(ini, L"Translate", L"id_table", true);
			c.force = ini_bool(ini, L"Translate", L"force", false);
			c.addressLibrary = ini_string(ini, L"Translate", L"address_library", L"");
			const auto spoof = ini_string(ini, L"Translate", L"legacy_runtime", L"1.6.1170.0");
			if (!Version::parse(spoof, c.spoof)) {
				logf("config: legacy_runtime \"%s\" is not a version; using 1.6.1170.0", narrow(spoof).c_str());
				c.spoof = Version{ { 1, 6, 1170, 0 } };
			}
			c.verifyIds = ini_bool(ini, L"Diagnostics", L"verify_ids", true);
			c.rttiScan = ini_bool(ini, L"Diagnostics", L"rtti_scan", true);
			c.faultLog = ini_bool(ini, L"Diagnostics", L"fault_log", true);
			c.messagingFanout = ini_bool(ini, L"Compatibility", L"messaging_fanout", true);
			logf("config: dry_run=%d legacy_dir=%s plugins=%s", c.dryRun ? 1 : 0, narrow(c.legacyDir).c_str(),
				c.plugins.empty() ? "*" : narrow(list).c_str());
			logf("config: translate=%d legacy_runtime=%s spoof_skse_runtime=%d id_table=%d force=%d address_library=%s",
				c.translate ? 1 : 0, c.spoof.str('.').c_str(), c.spoofSkseRuntime ? 1 : 0, c.idTable ? 1 : 0, c.force ? 1 : 0,
				c.addressLibrary.empty() ? "(auto)" : narrow(c.addressLibrary).c_str());
			logf("config: verify_ids=%d rtti_scan=%d fault_log=%d messaging_fanout=%d", c.verifyIds ? 1 : 0, c.rttiScan ? 1 : 0,
				c.faultLog ? 1 : 0, c.messagingFanout ? 1 : 0);
			return c;
		}

		std::vector<std::wstring> list_plugins(const Config& c)
		{
			std::vector<std::wstring> out;
			if (!c.plugins.empty()) {
				return c.plugins;
			}
			WIN32_FIND_DATAW fd{};
			const HANDLE     h = FindFirstFileW((c.legacyDir + L"*.dll").c_str(), &fd);
			if (h == INVALID_HANDLE_VALUE) {
				return out;
			}
			do {
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
					out.emplace_back(fd.cFileName);
				}
			} while (FindNextFileW(h, &fd));
			FindClose(h);
			std::sort(out.begin(), out.end());
			return out;
		}

		// Everything the translation needs, checked before any legacy DLL is loaded, so a
		// missing prerequisite becomes a log line instead of a fatal dialog from the plugin.
		Preflight preflight(const Config& c)
		{
			Preflight p;
			if (!exe_product_version(p.real)) {
				p.why = "cannot read the game exe version";
				return p;
			}
			const std::wstring pluginsDir = game_dir() + L"Data\\SKSE\\Plugins\\";

			// 1. The file the legacy plugin will open by itself once it believes it runs on the legacy runtime.
			const std::wstring legacyAl = pluginsDir + L"versionlib-" + c.spoof.wstr(L'-') + L".bin";
			AlHeader           lh;
			std::string        err;
			if (!load_al(legacyAl, lh, p.legacy, err)) {
				p.why = "legacy Address Library " + narrow(legacyAl) + ": " + err +
				        ". The legacy plugin reads this file's header itself; install the Address Library for " + c.spoof.str('.') +
				        " next to the current one.";
				return p;
			}
			logf("preflight: legacy Address Library %s: format=%d version=%s rows=%d", narrow(legacyAl).c_str(), lh.format,
				lh.version.str('.').c_str(), lh.count);
			if (lh.format != 2 || !(lh.version == c.spoof)) {
				p.why = "legacy Address Library has format " + std::to_string(lh.format) + " / version " + lh.version.str('.') +
				        "; the legacy plugin requires format 2 and version " + c.spoof.str('.');
				return p;
			}
			if (p.legacy.size() != static_cast<std::size_t>(lh.count)) {
				p.why = "legacy Address Library decoded " + std::to_string(p.legacy.size()) + " rows, header says " + std::to_string(lh.count);
				return p;
			}

			// 2. The Address Library that actually matches the running exe.
			std::wstring realAl = c.addressLibrary;
			if (realAl.empty()) {
				realAl = pluginsDir + L"versionlib-" + p.real.wstr(L'-') + L".bin";
			} else if (!(realAl.size() > 1 && realAl[1] == L':')) {
				realAl = game_dir() + realAl;
			}
			AlHeader rh;
			if (!load_al(realAl, rh, p.entries, err)) {
				p.why = "installed Address Library " + narrow(realAl) + ": " + err;
				return p;
			}
			logf("preflight: installed Address Library %s: format=%d version=%s pointerSize=%d dataFormat=%d count=%d usable IDs=%zu",
				narrow(realAl).c_str(), rh.format, rh.version.str('.').c_str(), rh.pointerSize, rh.dataFormat, rh.count,
				p.entries.size());
			if (!(rh.version == p.real)) {
				p.why = "installed Address Library is for " + rh.version.str('.') + " but the game is " + p.real.str('.');
				p.entries.clear();
				return p;
			}
			if (p.entries.empty()) {
				p.why = "installed Address Library contains no usable IDs";
				return p;
			}
			std::size_t carried = 0;
			for (const auto& e : p.legacy) {
				const auto it = std::lower_bound(p.entries.begin(), p.entries.end(), e.id, [](const IdEntry& x, std::uint64_t v) { return x.id < v; });
				carried += it != p.entries.end() && it->id == e.id;
			}
			logf("preflight: %zu of %zu legacy IDs exist in the installed library; %zu do not (they will fault if used)",
				carried, p.legacy.size(), p.legacy.size() - carried);
			const auto has = [](const std::vector<IdEntry>& v, std::uint64_t id) {
				const auto it = std::lower_bound(v.begin(), v.end(), id, [](const IdEntry& x, std::uint64_t i) { return x.id < i; });
				return it != v.end() && it->id == id;
			};
			for (const auto& [from, to] : known_remaps(c.spoof, p.real)) {
				if (has(p.legacy, from) && has(p.entries, to)) {
					p.baseRules.emplace_back(from, to);
					logf("preflight: known renumbered function: legacy ID %llu -> ID %llu", static_cast<unsigned long long>(from),
						static_cast<unsigned long long>(to));
				}
			}
			RemapRules global;
			const std::wstring globalIds = c.legacyDir + L"compat\\_global.ids";
			if (read_overlay(globalIds, global)) {
				logf("preflight: %zu rule(s) from %s", global.size(), narrow(globalIds).c_str());
				p.baseRules.insert(p.baseRules.end(), global.begin(), global.end());
			}
			p.ok = true;
			return p;
		}

		// Plain list of legacy IDs absent from the installed library, for grepping against plugin source.
		void write_missing_report(const Preflight& pf)
		{
			std::wstring path = log_dir() + L"SKSE-Bridge-missing-ids.txt";
			const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				return;
			}
			std::string out = "# Legacy Address Library IDs not present in the installed library.\r\n"
			                  "# A hosted plugin that uses any of these will fault at (image base + 0x0BAD000000000000 + id).\r\n"
			                  "# Remap with Legacy\\compat\\<plugin>.ids : legacyID = newID\r\n";
			for (const auto& e : pf.legacy) {
				const auto it = std::lower_bound(pf.entries.begin(), pf.entries.end(), e.id, [](const IdEntry& x, std::uint64_t v) { return x.id < v; });
				if (it == pf.entries.end() || it->id != e.id) {
					out += std::to_string(e.id);
					out += "\r\n";
				}
			}
			DWORD written = 0;
			WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
			CloseHandle(h);
			logf("preflight: missing-ID list written to %s", narrow(path).c_str());
		}

		using LoadFn = bool (*)(const SKSEInterface*);

		// Kept free of C++ objects so MSVC can use SEH here.
		bool call_load(LoadFn fn, const SKSEInterface* iface, bool* result, unsigned long* code)
		{
#if defined(_MSC_VER)
			__try {
				*result = fn(iface);
				return true;
			} __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
#else
			(void)code;
			*result = fn(iface);
			return true;
#endif
		}

		void host_one(const Config& c, const Preflight& pf, const SKSEInterface* skse, const std::wstring& file, const PeReport& rep)
		{
			const std::wstring path = c.legacyDir + file;
			logf("---- %s", narrow(path).c_str());
			if (!file_exists(path)) {
				logf("  SKIP: file not found");
				return;
			}
			log_report(rep);
			if (c.dryRun) {
				logf("  dry_run: not loaded");
				return;
			}
			if (!rep.ok || !rep.hasLoad) {
				logf("  SKIP: %s", rep.ok ? "no SKSEPlugin_Load export" : "not a loadable x64 DLL");
				return;
			}

			bool translate = c.translate;
			if (translate) {
				const bool hasVer = rep.imports_fn("GetFileVersionInfoW") && rep.imports_fn("VerQueryValueW");
				const bool hasMap = rep.imports_fn("OpenFileMappingW");
				if (!pf.ok && !c.force) {
					logf("  SKIP: translation prerequisites missing: %s", pf.why.c_str());
					return;
				}
				if ((!hasVer || !hasMap) && !c.force) {
					logf("  SKIP: this DLL does not import the functions the interposer relies on (version query=%d, OpenFileMappingW=%d). "
						 "It is not from the CommonLibSSE-NG lineage this host understands. Set force=1 to load it anyway.",
						hasVer ? 1 : 0, hasMap ? 1 : 0);
					return;
				}
				if (!pf.ok) {
					logf("  WARNING: force=1 with missing prerequisites (%s); loading without ID translation", pf.why.c_str());
				}
			}

			// Everything the interposer needs is prepared before LoadLibraryW, so it can be installed from the
			// loader notification, ahead of the plugin's DllMain and static initializers (earlyload.cpp).
			InterposeConfig ic;
			if (translate) {
				ic.spoofVersion = true;
				ic.spoof = c.spoof;
				if (pf.ok && c.idTable) {
					std::wstring stem = file;
					if (const auto dot = stem.find_last_of(L'.'); dot != std::wstring::npos) {
						stem.resize(dot);
					}
					const std::wstring overlay = c.legacyDir + L"compat\\" + stem + L".ids";
					RemapRules own;
					const bool hasOverlay = read_overlay(overlay, own);
					logf("  overlay: %s", hasOverlay ? (std::to_string(own.size()) + " rule(s) in " + narrow(overlay)).c_str() : "none");
					RemapRules rules = pf.baseRules;
					rules.insert(rules.end(), own.begin(), own.end());
					TableStats st;
					const auto table = build_legacy_table(pf.legacy, pf.entries, rules, st);
					logf("  table: %zu rows, %zu translated, %zu missing (poisoned), %zu remap rule(s) applied", st.rows,
						st.translated, st.missing, st.overlaid);
					check_ids(c.legacyDir + L"compat\\" + stem + L".check", pf.legacy, table);
					ic.mapping = make_mapping(table);
					ic.mappingBytes = table.size() * sizeof(IdEntry);
					if (!ic.mapping) {
						logf("  ERROR: could not create the private ID table (GetLastError=%lu)", static_cast<unsigned long>(GetLastError()));
					}
				}
			}

			early_arm(path, file, ic, translate, c.faultLog);
			logf("  loading (DllMain and static initializers run now)");
			SetLastError(0);
			const HMODULE     mod = LoadLibraryW(path.c_str());
			const DWORD       gle = GetLastError();
			const EarlyResult early = early_disarm();
			logf("  T0 LoadLibraryW -> %p (GetLastError=%lu)", static_cast<void*>(mod), static_cast<unsigned long>(mod ? 0 : gle));
			if (!mod) {
				return;
			}
			wchar_t loadedPath[1024]{};
			GetModuleFileNameW(mod, loadedPath, static_cast<DWORD>(std::size(loadedPath)));
			logf("  module path: %s", narrow(loadedPath).c_str());
			if (!early.patched) {
				skse_proxy_add_module(mod, narrow(file));
				if (c.faultLog) {
					fault_log_add_module(mod, narrow(file));
				}
			}
			if (c.rttiScan) {
				scan_rtti(mod, narrow(file));
			}

			if (translate) {
				const InterposeResult ir = early.patched ? early.r : install_interposer(mod, file, ic);
				logf("  interposer (%s): patched slots GetFileVersionInfoW=%d VerQueryValueW=%d GetFileVersionInfoA=%d VerQueryValueA=%d OpenFileMappingW=%d CreateFileMappingW=%d",
					early.patched ? "early, before DllMain" : "late, after DllMain", ir.getFileVersionInfoW, ir.verQueryValueW,
					ir.getFileVersionInfoA, ir.verQueryValueA, ir.openFileMappingW, ir.createFileMappingW);
				if (!early.patched) {
					logf("  note: %s; anything this plugin did with versions or IDs during DllMain/static init ran unpatched",
						!early.available ? "loader notifications unavailable" : (!early.sawModule ? "no loader notification for this module" : "imports were not yet bound at the notification"));
				}
			}

			const auto fn = reinterpret_cast<LoadFn>(reinterpret_cast<void*>(GetProcAddress(mod, "SKSEPlugin_Load")));
			if (!fn) {
				logf("  ERROR: GetProcAddress(SKSEPlugin_Load) failed (%lu)", static_cast<unsigned long>(GetLastError()));
				return;
			}

			// The legacy plugin gets a copy of the host's interface. Its plugin handle is the host's. SKSE keeps one
			// listener per handle and sender, so messaging goes through the bridge's fan-out (skseproxy.cpp).
			// Outgoing messages still carry the bridge's name; the co-save is still shared (warned, not fixed).
			auto* iface = new SKSEInterface(*skse);  // intentionally never freed; plugins keep the pointer
			if (const auto query = skse_proxy_query()) {
				iface->QueryInterface = query;  // messaging fan-out + co-save monitoring
				iface->GetPluginInfo = skse_proxy_plugin_info();  // hosted plugins can find each other by name
			}
			if (translate && c.spoofSkseRuntime) {
				iface->runtimeVersion = c.spoof.pack();
			}
			logf("  calling SKSEPlugin_Load (runtimeVersion presented: %s)", Version::unpack(iface->runtimeVersion).str('.').c_str());
			bool          result = false;
			unsigned long code = 0;
			skse_proxy_loading(narrow(file));
			const bool returned = call_load(fn, iface, &result, &code);
			skse_proxy_loading({});
			if (returned) {
				logf("  T1 SKSEPlugin_Load returned %s", result ? "true" : "false");
			} else {
				logf("  T1 SKSEPlugin_Load raised SEH exception 0x%08lX", code);
			}
		}
	}
}

extern "C"
{
	// [Assumption] Field requirements of the 1.7.99 SKSE loader were not inspected.
	// The host touches no game structs and resolves no addresses of its own, so it declares
	// itself version independent with no struct use. If skse64.log rejects the plugin, this
	// declaration is the first thing to adjust.
	__declspec(dllexport) bridge::PluginVersionData SKSEPlugin_Version = {
		bridge::PluginVersionData::kVersion,
		bridge::kHostVersion.pack(),
		"SKSE-Bridge",
		"Aboda7m",
		"",
		bridge::PluginVersionData::kVersionIndependentEx_NoStructUse,
		bridge::PluginVersionData::kVersionIndependent_AddressLibraryPostAE | bridge::PluginVersionData::kVersionIndependent_StructsPost629,
		{ 0 },
		0,
	};

	__declspec(dllexport) bool SKSEPlugin_Load(const bridge::SKSEInterface* skse)
	{
		using namespace bridge;
		log_open();
		logf("SKSE-Bridge %s (legacy plugin host)", kHostVersion.str('.').c_str());
		if (!skse) {
			logf("ERROR: null SKSE interface");
			return false;
		}
		Version exe{};
		const bool haveExe = exe_product_version(exe);
		logf("game exe: %s version %s", narrow(exe_path()).c_str(), haveExe ? exe.str('.').c_str() : "<unknown>");
		logf("SKSE interface: skseVersion=%s runtimeVersion=%s isEditor=%u", Version::unpack(skse->skseVersion).str('.').c_str(),
			Version::unpack(skse->runtimeVersion).str('.').c_str(), skse->isEditor);
		if (skse->isEditor) {
			return false;
		}

		const Config c = read_config();
		skse_proxy_init(skse, c.messagingFanout);
		logf("note: assuming SKSE does not scan sub-folders of Data\\SKSE\\Plugins [Assumption]. If skse64.log lists the legacy "
			 "DLLs itself, move legacy_dir outside Data\\SKSE\\Plugins.");

		const auto plugins = list_plugins(c);
		logf("legacy plugins: %zu", plugins.size());
		if (plugins.empty()) {
			return true;
		}

		Preflight pf;
		if (c.translate && !c.dryRun) {
			if (haveExe && exe == c.spoof) {
				logf("preflight: game is already %s; translation not needed, loading directly", exe.str('.').c_str());
			}
			pf = preflight(c);
			logf("preflight: %s%s", pf.ok ? "ok" : "FAILED: ", pf.ok ? "" : pf.why.c_str());
			if (pf.ok) {
				write_missing_report(pf);
				if (c.verifyIds) {
					TableStats st;
					verify_table(build_legacy_table(pf.legacy, pf.entries, pf.baseRules, st));
				}
			}
		}
		if (c.faultLog && !c.dryRun) {
			fault_log_install(pf.entries);
		}
		// Inspect everything first and declare every hosted plugin's name, so a plugin that looks for another
		// hosted plugin (messaging sender, GetPluginInfo) finds it regardless of load order.
		std::vector<PeReport> reports(plugins.size());
		for (std::size_t i = 0; i < plugins.size(); ++i) {
			const std::wstring path = c.legacyDir + plugins[i];
			if (!file_exists(path)) {
				continue;
			}
			reports[i] = inspect_pe(path);
			if (reports[i].ok && reports[i].hasVersionData) {
				char name[257]{};
				std::memcpy(name, reports[i].versionData.pluginName, 256);
				skse_proxy_declare(narrow(plugins[i]), name, reports[i].versionData.pluginVersion);
			}
		}
		for (std::size_t i = 0; i < plugins.size(); ++i) {
			host_one(c, pf, skse, plugins[i], reports[i]);
		}
		logf("done");
		return true;
	}
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
	return TRUE;
}
