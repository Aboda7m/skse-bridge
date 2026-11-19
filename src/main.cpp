// SKSE-Bridge: legacy plugin host.
//
// Loads SKSE plugins built for an older runtime (default 1.6.1170) on a newer one (1.7.99+),
// without modifying them. What it adapts: loading, the runtime version the legacy plugin sees,
// and Address Library ID lookups. What it cannot adapt: struct layouts, vtable slots, or any
// change in what the game code behind an ID does.
#include "common.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace bridge
{
	namespace
	{
		constexpr Version kHostVersion{ { 0, 7, 2, 0 } };

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
			bool                      patchAudit{ true };
			bool                      patchGuard{ true };
			bool                      presentPluginPath{ true };
			bool                      cosaveMux{ true };
			bool                      inferRemaps{ false };
			bool                      vmShadow{ true };      // [VMShadow] enabled: detect and offer the shadow
			bool                      vmAsk{ true };         // [VMShadow] ask_on_startup
			std::wstring              ini;
		};

		struct Preflight
		{
			bool                 ok{ false };
			std::string          why;
			Version              real{};
			std::vector<IdEntry> entries;      // the installed Address Library for the running game, sorted
			RemapRules           globalRules;  // compat\_global.ids, applied to every translated plugin
		};

		// Address Libraries by version, loaded on first use (one per plugin target), kept for the session.
		struct Lib
		{
			bool                 ok{ false };
			std::string          why;
			AlHeader             header;
			std::vector<IdEntry> entries;
		};

		const Lib& lib_for(const Version& v)
		{
			static std::map<std::uint32_t, Lib> libs;
			auto [it, fresh] = libs.try_emplace(v.pack());
			Lib& l = it->second;
			if (fresh) {
				const std::wstring path = game_dir() + L"Data\\SKSE\\Plugins\\versionlib-" + v.wstr(L'-') + L".bin";
				if (!load_al(path, l.header, l.entries, l.why)) {
					l.why = narrow(path) + ": " + l.why;
				} else if (!(l.header.version == v)) {
					l.why = narrow(path) + " is for " + l.header.version.str('.');
				} else {
					l.ok = true;
					logf("library: %s format=%d IDs=%zu", narrow(path).c_str(), l.header.format, l.entries.size());
				}
			}
			return l;
		}

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
			c.patchAudit = ini_bool(ini, L"Diagnostics", L"patch_audit", true);
			c.patchGuard = ini_bool(ini, L"Compatibility", L"patch_guard", true);
			c.presentPluginPath = ini_bool(ini, L"Compatibility", L"present_plugin_path", true);
			c.cosaveMux = ini_bool(ini, L"Compatibility", L"cosave_multiplex", true);
			c.inferRemaps = ini_bool(ini, L"Translate", L"infer_renumbered", false);
			c.ini = ini;
			c.vmShadow = ini_bool(ini, L"VMShadow", L"enabled", true);
			c.vmAsk = ini_bool(ini, L"VMShadow", L"ask_on_startup", true);
			logf("config: dry_run=%d legacy_dir=%s plugins=%s", c.dryRun ? 1 : 0, narrow(c.legacyDir).c_str(),
				c.plugins.empty() ? "*" : narrow(list).c_str());
			logf("config: translate=%d legacy_runtime=%s spoof_skse_runtime=%d id_table=%d force=%d address_library=%s",
				c.translate ? 1 : 0, c.spoof.str('.').c_str(), c.spoofSkseRuntime ? 1 : 0, c.idTable ? 1 : 0, c.force ? 1 : 0,
				c.addressLibrary.empty() ? "(auto)" : narrow(c.addressLibrary).c_str());
			logf("config: verify_ids=%d rtti_scan=%d fault_log=%d patch_audit=%d messaging_fanout=%d patch_guard=%d", c.verifyIds ? 1 : 0,
				c.rttiScan ? 1 : 0, c.faultLog ? 1 : 0, c.patchAudit ? 1 : 0, c.messagingFanout ? 1 : 0, c.patchGuard ? 1 : 0);
			logf("config: present_plugin_path=%d cosave_multiplex=%d vm_shadow=%d ask_on_startup=%d", c.presentPluginPath ? 1 : 0,
				c.cosaveMux ? 1 : 0, c.vmShadow ? 1 : 0, c.vmAsk ? 1 : 0);
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
		// The library of the running game, checked before any plugin is loaded.
		Preflight preflight(const Config& c)
		{
			Preflight p;
			if (!exe_product_version(p.real)) {
				p.why = "cannot read the game exe version";
				return p;
			}
			std::string err;
			AlHeader    rh;
			if (!c.addressLibrary.empty()) {
				const std::wstring realAl = (c.addressLibrary.size() > 1 && c.addressLibrary[1] == L':') ? c.addressLibrary : game_dir() + c.addressLibrary;
				if (!load_al(realAl, rh, p.entries, err)) {
					p.why = "installed Address Library " + narrow(realAl) + ": " + err;
					return p;
				}
			} else {
				const Lib& l = lib_for(p.real);
				if (!l.ok) {
					p.why = "Address Library for the running game: " + l.why;
					return p;
				}
				rh = l.header;
				p.entries = l.entries;
			}
			if (!(rh.version == p.real)) {
				p.why = "installed Address Library is for " + rh.version.str('.') + " but the game is " + p.real.str('.');
				p.entries.clear();
				return p;
			}
			logf("preflight: game %s, Address Library format %d with %zu IDs", p.real.str('.').c_str(), rh.format, p.entries.size());
			const std::wstring globalIds = c.legacyDir + L"compat\\_global.ids";
			if (read_overlay(globalIds, p.globalRules)) {
				logf("preflight: %zu rule(s) from %s", p.globalRules.size(), narrow(globalIds).c_str());
			}
			p.ok = !p.entries.empty();
			return p;
		}

		bool has_id(const std::vector<IdEntry>& v, std::uint64_t id)
		{
			const auto it = std::lower_bound(v.begin(), v.end(), id, [](const IdEntry& x, std::uint64_t i) { return x.id < i; });
			return it != v.end() && it->id == id;
		}

		// Plain list of the target's IDs absent from the game's library, for grepping against plugin source.
		void write_missing_report(const Version& target, const Lib& lib, const Preflight& pf)
		{
			const std::wstring path = log_dir() + L"SKSE-Bridge-missing-ids-" + target.wstr(L'-') + L".txt";
			const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				return;
			}
			std::string out = "# Address Library IDs of " + target.str('.') + " not present in the library of the running game (" + pf.real.str('.') +
			                  ").\r\n# A hosted plugin that uses one faults at (image base + 0x0BAD000000000000 + id) unless a rule remaps it.\r\n";
			for (const auto& e : lib.entries) {
				if (!has_id(pf.entries, e.id)) {
					out += std::to_string(e.id) + "\r\n";
				}
			}
			DWORD written = 0;
			WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
			CloseHandle(h);
			logf("  missing-ID list: %s", narrow(path).c_str());
		}

		// ---- shadow VM decisions: Data\SKSE\Plugins\SKSE-Bridge\ShadowVM\<dll>.json, one file per plugin ----
		// Easy to find, edit or delete with the mod (under MO2 they land in Overwrite). Deleting a file = ask again.
		// The file records the DLL's SHA-256 prefix: an updated DLL is asked about again.
		// _session_open.txt exists while a session with a shadowed plugin has not reached a loaded game.
		SRWLOCK g_decLock = SRWLOCK_INIT;

		std::wstring shadow_dir()
		{
			const std::wstring root = game_dir() + L"Data\\SKSE\\Plugins\\SKSE-Bridge\\";
			CreateDirectoryW(root.c_str(), nullptr);
			const std::wstring dir = root + L"ShadowVM\\";
			CreateDirectoryW(dir.c_str(), nullptr);
			return dir;
		}

		bool write_text(const std::wstring& path, const std::string& text)
		{
			const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				return false;
			}
			DWORD w = 0;
			WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &w, nullptr);
			CloseHandle(h);
			return true;
		}

		// "key": "string" or "key": true|false from a small flat JSON text.
		std::string json_value(const std::string& t, const std::string& key)
		{
			const auto k = t.find("\"" + key + "\"");
			if (k == std::string::npos) {
				return {};
			}
			auto v = t.find_first_not_of(" \t\r\n:", k + key.size() + 2);
			if (v == std::string::npos) {
				return {};
			}
			if (t[v] == '"') {
				const auto e = t.find('"', v + 1);
				return e == std::string::npos ? std::string{} : t.substr(v + 1, e - v - 1);
			}
			const auto e = t.find_first_of(",}\r\n", v);
			std::string r = t.substr(v, e == std::string::npos ? std::string::npos : e - v);
			while (!r.empty() && (r.back() == ' ' || r.back() == '\t')) {
				r.pop_back();
			}
			return r;
		}

		// Saved answer for this DLL build: 1 yes, 0 no, -1 none. key: "shadow_vm" or "shadow_player".
		int read_decision(const std::wstring& file, const std::string& sha, const char* key)
		{
			std::vector<std::uint8_t> raw;
			std::string               err;
			if (!read_file(shadow_dir() + file + L".json", raw, err)) {
				return -1;
			}
			const std::string t(raw.begin(), raw.end());
			const std::string saved = json_value(t, "sha256");
			if (!saved.empty() && !sha.empty() && saved.substr(0, 16) != sha.substr(0, 16)) {
				logf("  shadow: saved answers are for another build of %s (sha256 %s...); asking again", narrow(file).c_str(), saved.substr(0, 16).c_str());
				return -1;
			}
			const std::string v = json_value(t, key);
			return v == "true" ? 1 : (v == "false" ? 0 : -1);
		}

		void write_decision(const std::wstring& file, const std::string& sha, const char* key, bool on)
		{
			int vm = read_decision(file, sha, "shadow_vm");
			int pl = read_decision(file, sha, "shadow_player");
			(std::string(key) == "shadow_vm" ? vm : pl) = on ? 1 : 0;
			std::string text = "{\r\n  \"plugin\": \"" + narrow(file) + "\",\r\n  \"sha256\": \"" + sha + "\",\r\n";
			if (vm >= 0) {
				text += std::string("  \"shadow_vm\": ") + (vm ? "true" : "false") + ",\r\n";
			}
			if (pl >= 0) {
				text += std::string("  \"shadow_player\": ") + (pl ? "true" : "false") + ",\r\n";
			}
			text += "  \"note\": \"Written by SKSE-Bridge. shadow_vm / shadow_player: true = run this plugin with that shadow, false = load it normally. Delete this file to be asked again.\"\r\n}\r\n";
			write_text(shadow_dir() + file + L".json", text);
		}

		std::wstring session_marker() { return shadow_dir() + L"_session_open.txt"; }
		bool         g_anyShadow = false;  // a plugin runs with a shadow this session

		// Shadow decision for one plugin and class: ini override, then the saved answer, then a question.
		bool decide_shadow(const Config& c, ShadowClass cls, const std::wstring& file, const PeReport& rep, bool plugin17, bool game17, bool lastCrashed,
			const std::string& why)
		{
			const bool        vm = cls == kShadowSkyrimVM;
			const wchar_t*    section = vm ? L"VMShadow" : L"PlayerShadow";
			const char*       key = vm ? "shadow_vm" : "shadow_player";
			const char*       tag = vm ? "vm-shadow" : "player-shadow";
			const std::wstring over = ini_string(c.ini, section, file.c_str(), L"");
			if (!over.empty()) {
				const bool on = over == L"1" || iequals(over, L"true") || iequals(over, L"yes");
				logf("  %s: %s by [%s] %s", tag, on ? "ON" : "off", narrow(section).c_str(), narrow(file).c_str());
				return on;
			}
			const int saved = read_decision(file, rep.sha256, key);
			if (saved >= 0 && !(lastCrashed && saved == 1)) {
				logf("  %s: %s (saved answer in %s%s.json)", tag, saved ? "ON" : "off", narrow(shadow_dir()).c_str(), narrow(file).c_str());
				return saved == 1;
			}
			if (saved == 1) {
				logf("  %s: saved answer is Yes, but the last launch with a shadow did not reach the main menu (%s exists): asking again", tag,
					narrow(session_marker()).c_str());
			} else {
				logf("  %s: no usable saved answer at %s%s.json", tag, narrow(shadow_dir()).c_str(), narrow(file).c_str());
			}
			if (!c.vmAsk) {
				logf("  %s: off (undecided and ask_on_startup=0)", tag);
				return false;
			}
			std::wstring text = file + (vm ? L" uses Skyrim's Papyrus VM object (SkyrimVM)" : L" uses the player object (PlayerCharacter: " + widen(why) + L")") +
			                    L", which changed in 1.7. This DLL was built for the " + std::wstring(plugin17 ? L"1.7" : L"1.6") + L" layout and the game is " +
			                    (game17 ? L"1.7" : L"1.6") + (vm ? L". Without help, its Papyrus functions can fail or crash.\n\n" :
			                                                       L". Without help, registering for those events writes into the wrong place (hang or crash).\n\n");
			if (lastCrashed) {
				text += L"Note: the last launch with a shadow did not reach the main menu.\n\n";
			}
			text += std::wstring(L"Run it with a shadow ") + (vm ? L"VM" : L"player") +
			        L" (EXPERIMENTAL)?\n\nYes = use the shadow\nNo = load it normally\nCancel = do not load this plugin this session (asked again next launch)\n\n"
			        L"Your choice is saved in Data\\SKSE\\Plugins\\SKSE-Bridge\\ShadowVM\\" + file + L".json (delete it to be asked again).";
			const int r = MessageBoxW(nullptr, text.c_str(), L"SKSE-Bridge", MB_YESNOCANCEL | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND);
			if (r == IDCANCEL) {
				logf("  %s: user chose Cancel: plugin not loaded this session", tag);
				throw std::runtime_error("cancelled");
			}
			const bool on = r == IDYES;
			AcquireSRWLockExclusive(&g_decLock);
			write_decision(file, rep.sha256, key, on);
			ReleaseSRWLockExclusive(&g_decLock);
			logf("  %s: %s (asked; saved to %s%s.json)", tag, on ? "ON" : "off", narrow(shadow_dir()).c_str(), narrow(file).c_str());
			return on;
		}

		HANDLE mapping_from_bytes(const void* data, std::size_t bytes)
		{
			const HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(static_cast<std::uint64_t>(bytes) >> 32),
				static_cast<DWORD>(bytes), nullptr);
			if (!h) {
				return nullptr;
			}
			void* v = MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, bytes);
			if (!v) {
				CloseHandle(h);
				return nullptr;
			}
			std::memcpy(v, data, bytes);
			UnmapViewOfFile(v);
			return h;
		}

		// commonlib-shared table for the running game, with the SkyrimVM singleton ID pointed at the shadow global.
		// Format 2: sorted {id, offset} pairs (MAPPING, 16 bytes). Format 5: the file itself (header + u32 offsets).
		HANDLE shared_table_with_shadow(const Preflight& pf, unsigned mask, std::size_t& bytes)
		{
			const std::wstring path = game_dir() + L"Data\\SKSE\\Plugins\\versionlib-" + pf.real.wstr(L'-') + L".bin";
			const Lib&         lib = lib_for(pf.real);
			if (!lib.ok) {
				return nullptr;
			}
			if (lib.header.format == 5) {
				std::vector<std::uint8_t> raw;
				std::string               err;
				if (!read_file(path, raw, err)) {
					return nullptr;
				}
				for (int k = 0; k < kShadowClassCount; ++k) {
					const auto gOff = shadow_global_offset(static_cast<ShadowClass>(k));
					const std::size_t at = 96 + 4 * shadow_id(static_cast<ShadowClass>(k));
					if (!(mask & (1u << k))) {
						continue;
					}
					if (!gOff || gOff > 0xFFFFFFFFull || at + 4 > raw.size()) {
						return nullptr;
					}
					*reinterpret_cast<std::uint32_t*>(&raw[at]) = static_cast<std::uint32_t>(gOff);
				}
				bytes = raw.size();
				return mapping_from_bytes(raw.data(), raw.size());
			}
			std::vector<IdEntry> t = lib.entries;
			for (auto& e : t) {
				for (int k = 0; k < kShadowClassCount; ++k) {
					if ((mask & (1u << k)) && e.id == shadow_id(static_cast<ShadowClass>(k))) {
						e.offset = shadow_global_offset(static_cast<ShadowClass>(k));
					}
				}
			}
			bytes = t.size() * sizeof(IdEntry);
			return mapping_from_bytes(t.data(), bytes);
		}

		struct Plan
		{
			Version     target{};
			bool        translate{ false };
			std::string why;
		};

		// Which runtime a hosted DLL was built for, and whether the bridge translates it.
		Plan plan_for(const Config& c, const Preflight& pf, const PeReport& rep, const std::wstring& file)
		{
			Plan       p;
			const bool ngLegacy = std::string(rep.lineage()) == "A" || std::string(rep.lineage()) == "B";
			const bool ngImports = rep.imports_fn("GetFileVersionInfoW") && rep.imports_fn("VerQueryValueW") && rep.imports_fn("OpenFileMappingW");
			// Same rule as 0.5.0: the imports the interposer answers decide; the strings only refine (commonlib-shared
			// validates and maps its own library, so it is never fed a translated one).
			const bool translatable = !rep.strIddbShared && ((ngImports && (ngLegacy || std::string(rep.lineage()) == "unknown")) || (ngLegacy && c.force));
			p.target = pf.real;

			const std::wstring over = ini_string(c.ini, L"Targets", file.c_str(), L"");
			if (!over.empty()) {
				if (iequals(over, L"native")) {
					p.why = "[Targets] native";
					return p;
				}
				Version v;
				if (Version::parse(over, v)) {
					p.target = v;
					p.translate = translatable && !(v == pf.real);
					p.why = "[Targets] " + v.str('.');
					if (!translatable && !(v == pf.real)) {
						p.why += " (this DLL's Address Library loader is not one the bridge can answer: loaded natively, target used for diagnostics only)";
					}
					return p;
				}
				logf("  [Targets] %s: \"%s\" is not a version or 'native'; ignored", narrow(file).c_str(), narrow(over).c_str());
			}

			const auto& vd = rep.versionData;
			const bool  independent = vd.versionIndependence & (PluginVersionData::kVersionIndependent_AddressLibraryPostAE | PluginVersionData::kVersionIndependent_Signatures);
			if (rep.hasVersionData && !independent && vd.compatibleVersions[0]) {
				for (const auto cv : vd.compatibleVersions) {
					if (cv && Version::unpack(cv) == pf.real) {
						p.why = "declares compatibility with this game version";
						return p;
					}
				}
				p.target = Version::unpack(vd.compatibleVersions[0]);
				p.translate = translatable;
				p.why = "declares compatibility with " + p.target.str('.');
				return p;
			}
			if (translatable) {
				p.target = c.spoof;
				p.translate = !(c.spoof == pf.real);
				p.why = "legacy CommonLibSSE-NG (reads its runtime from the exe): legacy_runtime";
				return p;
			}
			p.why = std::string("lineage ") + rep.lineage() + ": loads its own Address Library for the running game";
			return p;
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

			Plan plan = plan_for(c, pf, rep, file);
			if (!c.translate || !pf.ok) {
				plan.translate = false;
			}
			// [PresentVersion] <dll>=<version>: the runtime version the SKSE interface reports to this plugin only, for plugins
			// whose own code refuses unknown versions (e.g. a list of 1.7 versions) but that load the Address Library of the
			// running game themselves. Nothing else is spoofed; the plugin's compiled structure layouts are unchanged.
			Version presentVersion{};
			bool    hasPresent = false;
			if (const std::wstring pv = ini_string(c.ini, L"PresentVersion", file.c_str(), L""); !pv.empty()) {
				if (Version::parse(pv, presentVersion)) {
					hasPresent = true;
					if (!plan.translate) {
						plan.target = presentVersion;  // diagnostics (vtable direction, VM layout default) follow the claimed build
					}
					logf("  [PresentVersion] SKSE interface will report %s to this plugin (EXPERIMENTAL; its structure layouts are those of its own build)",
						presentVersion.str('.').c_str());
				} else {
					logf("  [PresentVersion] %s: \"%s\" is not a version; ignored", narrow(file).c_str(), narrow(pv).c_str());
				}
			}
			const int direction = vtable_direction(plan.target, pf.ok ? pf.real : plan.target);
			logf("  target: built for %s, game %s -> %s (%s)", plan.target.str('.').c_str(), pf.real.str('.').c_str(),
				plan.translate ? "TRANSLATED" : "loaded natively", plan.why.c_str());

			std::wstring stem = file;
			if (const auto dot = stem.find_last_of(L'.'); dot != std::wstring::npos) {
				stem.resize(dot);
			}
			const Lib* targetLib = nullptr;
			if (pf.ok && !(plan.target == pf.real)) {
				const Lib& l = lib_for(plan.target);
				if (l.ok) {
					targetLib = &l;
				} else {
					logf("  target library unavailable: %s", l.why.c_str());
				}
			}
			if (plan.translate && (!targetLib || targetLib->header.format != 2)) {
				logf("  SKIP: translation for %s needs its Address Library in format 2 (%s). Install the Address Library that "
					 "covers %s, or set [Targets] %s=native.",
					plan.target.str('.').c_str(), targetLib ? ("found format " + std::to_string(targetLib->header.format)).c_str() : "not found",
					plan.target.str('.').c_str(), narrow(file).c_str());
				return;
			}

			// Shadow objects (vmshadow.cpp): static evidence, then one decision per class.
			unsigned shadowMask = 0;
			bool     plugin17 = plan.target.at_least(1, 7, 99);
			if (pf.ok && c.vmShadow) {
				const VmScan vs = scan_vm_layout(path);
				if (vs.layout) {
					plugin17 = vs.layout == 17;
				}
				const bool game17 = pf.real.at_least(1, 7, 99);
				if (vs.idRefs) {
					std::string sites;
					for (std::size_t i = 0; i < vs.sites.size() && i < 8; ++i) {
						char b[24];
						std::snprintf(b, sizeof(b), " +0x%X", vs.sites[i]);
						sites += b;
					}
					logf("  SkyrimVM: %u reference(s) to the singleton ID; reads at +0x200: %u, +0x210: %u ->%s layout %s (%s)%s", vs.idRefs, vs.votes16,
						vs.votes17, vs.layout ? "" : " assumed", plugin17 ? "1.7" : "1.6", vs.layout ? "code evidence" : "from target version", sites.c_str());
				}
				std::string sinks;
				for (const auto& e : vs.playerSinks) {
					sinks += (sinks.empty() ? "" : ", ") + e;
				}
				if (vs.playerRefs && !vs.playerSinks.empty()) {
					logf("  PlayerCharacter: %u reference(s) to the singleton ID; sinks for player event sources that moved in 1.7.99: %s (layout %s)",
						vs.playerRefs, sinks.c_str(), plugin17 ? "1.7" : "1.6");
				}
				const bool wantVm = vs.idRefs && plugin17 != game17;
				const bool wantPlayer = vs.playerRefs && !vs.playerSinks.empty() && plugin17 != game17;
				if (vs.idRefs && !wantVm) {
					logf("  SkyrimVM: same layout as the game; no shadow needed");
				}
				if (wantVm || wantPlayer) {
					const bool lastCrashed = file_exists(session_marker());
					try {
						if (wantVm && decide_shadow(c, kShadowSkyrimVM, file, rep, plugin17, game17, lastCrashed, {}) &&
							shadow_init(kShadowSkyrimVM, pf.real, pf.entries)) {
							shadowMask |= 1u << kShadowSkyrimVM;
						}
						if (wantPlayer && decide_shadow(c, kShadowPlayer, file, rep, plugin17, game17, lastCrashed, "event sinks " + sinks) &&
							shadow_init(kShadowPlayer, pf.real, pf.entries)) {
							shadowMask |= 1u << kShadowPlayer;
						}
					} catch (const std::exception&) {
						return;
					}
					if (shadowMask) {
						write_text(session_marker(), "A launch with a shadowed plugin has not reached the main menu (kDataLoaded) yet.\r\n");
						g_anyShadow = true;
					}
				}
			}
			bool shadowVm = shadowMask != 0;

			InterposeConfig ic;
			ic.spoofVersion = false;  // set below when translated
			ic.realPath = path;
			if (c.presentPluginPath) {
				ic.presentPath = game_dir() + L"Data\\SKSE\\Plugins\\" + file;
				logf("  module path presented to the plugin: %s", narrow(ic.presentPath).c_str());
			}
			ic.patchAudit = pf.ok && c.patchAudit;
			if (ic.patchAudit && targetLib) {
				patch_audit_target(narrow(file), plan.target, targetLib->entries, direction);
			}
			if (plan.translate) {
				ic.spoofVersion = true;
				ic.spoof = plan.target;
				if (c.idTable) {
					RemapRules rules;
					unsigned   known = 0, inferred = 0;
					for (const auto& r : known_remaps(plan.target, pf.real)) {
						if (has_id(targetLib->entries, r.first) && has_id(pf.entries, r.second)) {
							rules.push_back(r);
							++known;
						}
					}
					if (c.inferRemaps) {
						for (const auto& r : infer_remaps(targetLib->entries, pf.entries)) {
							const bool dup = std::any_of(rules.begin(), rules.end(), [&](const auto& x) { return x.first == r.first; });
							if (!dup) {
								rules.push_back(r);
								++inferred;
							}
						}
					}
					rules.insert(rules.end(), pf.globalRules.begin(), pf.globalRules.end());
					const std::wstring overlay = c.legacyDir + L"compat\\" + stem + L".ids";
					RemapRules own;
					const bool hasOverlay = read_overlay(overlay, own);
					rules.insert(rules.end(), own.begin(), own.end());
					logf("  remaps: %u known, %u inferred (infer_renumbered=%d), %zu global, %zu from %s", known, inferred, c.inferRemaps ? 1 : 0,
						pf.globalRules.size(), own.size(), hasOverlay ? narrow(overlay).c_str() : "no overlay");
					TableStats st;
					auto       table = build_legacy_table(targetLib->entries, pf.entries, rules, st);
					for (int k = 0; k < kShadowClassCount; ++k) {
						if (!(shadowMask & (1u << k))) {
							continue;
						}
						for (auto& e : table) {
							if (e.id == shadow_id(static_cast<ShadowClass>(k))) {
								e.offset = shadow_global_offset(static_cast<ShadowClass>(k));
								logf("  shadow: ID %llu (%s) in this plugin's table -> shadow global", static_cast<unsigned long long>(e.id),
									shadow_name(static_cast<ShadowClass>(k)));
							}
						}
					}
					logf("  table: %zu rows, %zu translated, %zu missing (poisoned), %zu remap rule(s) applied", st.rows, st.translated,
						st.missing, st.overlaid);
					write_missing_report(plan.target, *targetLib, pf);
					check_ids(c.legacyDir + L"compat\\" + stem + L".check", targetLib->entries, table);
					if (c.verifyIds) {
						verify_table(table);
					}
					ic.mapping = make_mapping(table);
					ic.mappingBytes = table.size() * sizeof(IdEntry);
					ic.idTable = ic.mapping != nullptr;
					if (!ic.mapping) {
						logf("  ERROR: could not create the private ID table (GetLastError=%lu)", static_cast<unsigned long>(GetLastError()));
					}
				}
			}

			if (ic.patchAudit) {
				const std::wstring allowPath = c.legacyDir + L"compat\\" + stem + L".patches";
				std::vector<std::uint8_t> raw;
				std::string               err;
				if (file_exists(allowPath) && read_file(allowPath, raw, err)) {
					std::vector<std::pair<std::uint64_t, std::uint64_t>> sites;
					std::string                                          text(raw.begin(), raw.end());
					std::size_t                                          pos = 0;
					while (pos < text.size()) {
						auto        eol = text.find('\n', pos);
						std::string ln = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
						pos = eol == std::string::npos ? text.size() : eol + 1;
						unsigned long long id = 0, off = 0;
						if (std::sscanf(ln.c_str(), " allow %llu + %llx", &id, &off) == 2) {
							sites.emplace_back(id, off);
						}
					}
					patch_audit_allow(narrow(file), sites);
					logf("  patch allow-list: %zu site(s) from %s", sites.size(), narrow(allowPath).c_str());
				}
			}

			if (shadowVm && !plan.translate) {
				if (rep.strIddbShared) {
					ic.sharedMapping = shared_table_with_shadow(pf, shadowMask, ic.sharedBytes);
					logf("  shadow: commonlib-shared table for %s %s", pf.real.str('.').c_str(), ic.sharedMapping ? "prepared" : "FAILED (shadow off)");
				}
				if (!ic.sharedMapping) {
					logf("  shadow: the bridge does not answer this plugin's Address Library loader; shadow not possible");
					shadowVm = false;
					shadowMask = 0;
				}
			}

			// The interposer is installed for every hosted DLL (path, patch audit); version spoof and ID table only when translated.
			early_arm(path, file, ic, true, c.faultLog);
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
			if (shadowVm) {
				shadow_add_module(mod, narrow(file), plugin17, shadowMask);
			}
			if (c.rttiScan) {
				scan_rtti(mod, narrow(file), direction);
			}

			const InterposeResult ir = early.patched ? early.r : install_interposer(mod, file, ic);
			logf("  interposer (%s): version W=%d/%d A=%d/%d id-table=%d/%d module-path=%d VirtualProtect=%d",
				early.patched ? "early, before DllMain" : "late, after DllMain", ir.getFileVersionInfoW, ir.verQueryValueW, ir.getFileVersionInfoA,
				ir.verQueryValueA, ir.openFileMappingW, ir.createFileMappingW, ir.modulePath, ir.virtualProtect);
			if (!early.patched && plan.translate) {
				logf("  note: %s; anything this plugin did with versions or IDs during DllMain/static init ran unpatched",
					!early.available ? "loader notifications unavailable" : (!early.sawModule ? "no loader notification for this module" : "imports were not yet bound at the notification"));
			}

			const auto fn = reinterpret_cast<LoadFn>(reinterpret_cast<void*>(GetProcAddress(mod, "SKSEPlugin_Load")));
			if (!fn) {
				logf("  ERROR: GetProcAddress(SKSEPlugin_Load) failed (%lu)", static_cast<unsigned long>(GetLastError()));
				return;
			}

			// The legacy plugin gets a copy of the host's interface. Its plugin handle is the host's. SKSE keeps one
			// listener per handle and sender, so messaging goes through the bridge's fan-out (skseproxy.cpp).
			auto* iface = new SKSEInterface(*skse);  // intentionally never freed; plugins keep the pointer
			if (const auto query = skse_proxy_query()) {
				iface->QueryInterface = query;
				iface->GetPluginInfo = skse_proxy_plugin_info();
			}
			if (plan.translate && c.spoofSkseRuntime) {
				iface->runtimeVersion = plan.target.pack();
			}
			if (hasPresent) {
				iface->runtimeVersion = presentVersion.pack();
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
			if (ic.patchAudit) {
				patch_audit_flush("after SKSEPlugin_Load", narrow(file));
			}
		}
	}
}

namespace bridge
{
	void vm_session_reached_game()
	{
		const std::wstring m = session_marker();
		if (file_exists(m)) {
			DeleteFileW(m.c_str());
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
		// AddressLibraryV5: the host reads the 1.7.x (format 5) Address Library itself. Without it SKSE 2.3.1 rejects
		// the DLL when the linker's TimeDateStamp happens to be a date before 2025-05-26 (lld and MSVC /Brepro write a
		// hash there, so this varied between builds: 0.4.0 loaded, 0.5.0 did not).
		bridge::PluginVersionData::kVersionIndependentEx_NoStructUse | bridge::PluginVersionData::kVersionIndependentEx_AddressLibraryV5,
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
		skse_proxy_init(skse, c.messagingFanout, c.cosaveMux && !c.dryRun);
		logf("note: assuming SKSE does not scan sub-folders of Data\\SKSE\\Plugins [Assumption]. If skse64.log lists the legacy "
			 "DLLs itself, move legacy_dir outside Data\\SKSE\\Plugins.");

		const auto plugins = list_plugins(c);
		logf("legacy plugins: %zu", plugins.size());
		if (plugins.empty()) {
			return true;
		}

		Preflight pf;
		if (!c.dryRun) {
			pf = preflight(c);
			logf("preflight: %s%s", pf.ok ? "ok" : "FAILED: ", pf.ok ? "" : pf.why.c_str());
			if (pf.ok && c.patchAudit) {
				patch_audit_init(pf.entries, c.patchGuard);
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
		if (!g_anyShadow && file_exists(game_dir() + L"Data\\SKSE\\Plugins\\SKSE-Bridge\\ShadowVM\\_session_open.txt")) {
			DeleteFileW((game_dir() + L"Data\\SKSE\\Plugins\\SKSE-Bridge\\ShadowVM\\_session_open.txt").c_str());  // nothing shadowed this session
		}
		logf("done");
		return true;
	}
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
	return TRUE;
}
