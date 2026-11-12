// SKSE-Bridge legacy plugin host - shared declarations.
// No CommonLib dependency. Win32 + C++20 only.
#pragma once

#ifndef _WIN32_WINNT
#	define _WIN32_WINNT 0x0A00  // GetCurrentThreadStackLimits
#endif
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cwchar>
#include <iterator>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace bridge
{
	// ---- SKSE ABI (layouts transcribed from CommonLibSSE-NG 539d4ce, MIT:
	//      include/SKSE/Impl/Stubs.h:19-36, include/SKSE/Interfaces.h:360-445) ----
	struct SKSEInterface
	{
		std::uint32_t skseVersion;
		std::uint32_t runtimeVersion;
		std::uint32_t editorVersion;
		std::uint32_t isEditor;
		void* (*QueryInterface)(std::uint32_t);
		std::uint32_t (*GetPluginHandle)();
		std::uint32_t (*GetReleaseIndex)();
		const void* (*GetPluginInfo)(const char*);
	};

	struct PluginVersionData
	{
		enum : std::uint32_t
		{
			kVersion = 1,
			kVersionIndependent_AddressLibraryPostAE = 1u << 0,
			kVersionIndependent_Signatures = 1u << 1,
			kVersionIndependent_StructsPost629 = 1u << 2,
			kVersionIndependentEx_NoStructUse = 1u << 0,
		};

		std::uint32_t dataVersion;
		std::uint32_t pluginVersion;
		char          pluginName[256];
		char          author[256];
		char          supportEmail[252];
		std::uint32_t versionIndependenceEx;
		std::uint32_t versionIndependence;
		std::uint32_t compatibleVersions[16];
		std::uint32_t xseMinimum;
	};
	static_assert(sizeof(PluginVersionData) == 0x350);
	static_assert(offsetof(PluginVersionData, versionIndependenceEx) == 0x304);
	static_assert(offsetof(PluginVersionData, compatibleVersions) == 0x30C);

	struct Version
	{
		std::uint16_t v[4]{};

		// Same packing as REL::Version::pack (NG 539d4ce include/REL/Version.h:70-77).
		[[nodiscard]] constexpr std::uint32_t pack() const noexcept
		{
			return (std::uint32_t(v[0] & 0x0FF) << 24) | (std::uint32_t(v[1] & 0x0FF) << 16) |
			       (std::uint32_t(v[2] & 0xFFF) << 4) | std::uint32_t(v[3] & 0x00F);
		}
		[[nodiscard]] bool operator==(const Version& o) const noexcept { return std::memcmp(v, o.v, sizeof(v)) == 0; }
		[[nodiscard]] bool less(const Version& o) const noexcept
		{
			for (int i = 0; i < 4; ++i) {
				if (v[i] != o.v[i]) {
					return v[i] < o.v[i];
				}
			}
			return false;
		}
		[[nodiscard]] std::wstring wstr(wchar_t sep) const;
		[[nodiscard]] std::string  str(char sep) const;
		static bool                parse(std::wstring_view text, Version& out);
		static Version             unpack(std::uint32_t packed);
	};

	// ---- util.cpp ----
	void         log_open();
	std::wstring log_dir();  // SKSE log folder, with trailing backslash
	void         logf(const char* fmt, ...);
	std::string  narrow(std::wstring_view w);
	std::wstring widen(std::string_view s);
	std::wstring exe_path();
	std::wstring game_dir();  // directory of the game exe, with trailing backslash
	bool         exe_product_version(Version& out);
	bool         read_file(const std::wstring& path, std::vector<std::uint8_t>& out, std::string& err);
	bool         file_exists(const std::wstring& path);
	std::wstring base_name(std::wstring_view path);
	bool         iequals(std::wstring_view a, std::wstring_view b);

	// ---- pe.cpp : read-only inspection of a DLL file on disk ----
	struct PeImport
	{
		std::string              dll;
		std::vector<std::string> names;
	};

	struct PeReport
	{
		bool                     ok{ false };
		std::string              error;
		std::uint64_t            fileSize{ 0 };
		std::string              sha256;
		bool                     x64{ false };
		std::uint32_t            timeDateStamp{ 0 };
		bool                     hasPdb{ false };
		std::string              pdbGuid;
		std::uint32_t            pdbAge{ 0 };
		std::string              pdbPath;
		std::vector<std::string> exports;
		std::vector<PeImport>    imports;
		bool                     hasLoad{ false };
		bool                     hasVersionData{ false };
		PluginVersionData        versionData{};
		bool                     strOffsetsMap{ false };  // "CommonLibSSEOffsets"
		bool                     strVersionlib{ false };  // "versionlib-"
		bool                     strVersionSE{ false };   // "Plugins/version-"
		bool                     strSkseRuntime{ false }; // "SKSE_RUNTIME"

		[[nodiscard]] bool        imports_fn(std::string_view fn) const;
		[[nodiscard]] const char* lineage() const;  // "A", "B" or "unknown"
	};

	PeReport inspect_pe(const std::wstring& path);
	void     log_report(const PeReport& r);

	// ---- idtable.cpp : Address Library readers and the private table ----
	struct IdEntry
	{
		std::uint64_t id;
		std::uint64_t offset;
	};
	static_assert(sizeof(IdEntry) == 16);

	struct AlHeader
	{
		std::int32_t format{ 0 };
		Version      version{};
		std::int32_t pointerSize{ 0 };
		std::int32_t dataFormat{ 0 };  // format 5 only
		std::int32_t count{ 0 };
		std::size_t  dataOffset{ 0 };
	};

	bool read_al_header(const std::wstring& path, AlHeader& hdr, std::string& err);
	bool load_al(const std::wstring& path, AlHeader& hdr, std::vector<IdEntry>& out, std::string& err);

	// compat/<plugin>.ids : "legacyID = newID" lines. Returns false if the file does not exist.
	bool read_overlay(const std::wstring& path, std::vector<std::pair<std::uint64_t, std::uint64_t>>& rules);

	// Offset given to a legacy ID that the installed Address Library does not contain.
	// base + this is a non-canonical address: any read, write or call faults at once, and the
	// faulting address minus the image base shows the ID. Never a silently wrong function.
	constexpr std::uint64_t kMissingOffset = 0x0BAD000000000000ull;

	struct TableStats
	{
		std::size_t rows{ 0 };
		std::size_t translated{ 0 };
		std::size_t overlaid{ 0 };
		std::size_t missing{ 0 };
	};

	// One row per ID of the legacy (1.6.1170) library, in the same sorted order, so the row count
	// matches the address_count the legacy plugin read from that file's header.
	// Offsets come from the installed library; overlay rules may redirect individual IDs.
	std::vector<IdEntry> build_legacy_table(const std::vector<IdEntry>& legacy, const std::vector<IdEntry>& current,
		const std::vector<std::pair<std::uint64_t, std::uint64_t>>& rules, TableStats& stats);

	HANDLE make_mapping(const std::vector<IdEntry>& table);

	using RemapRules = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

	// Functions that 1.7.99 renumbered (old ID absent from the 1.7.99 library, same function under a new ID).
	// Empty unless legacy is 1.6.x and the game is 1.7.99 or later.
	RemapRules known_remaps(const Version& legacy, const Version& game);

	// ---- verify.cpp : checks resolved offsets against the running SkyrimSE.exe image ----
	void verify_table(const std::vector<IdEntry>& table);
	// compat/<plugin>.check : IDs the plugin uses (any text; every number is taken as an ID).
	void check_ids(const std::wstring& path, const std::vector<IdEntry>& legacy, const std::vector<IdEntry>& table);
	[[nodiscard]] std::uintptr_t exe_base();
	[[nodiscard]] std::uint32_t  exe_size();

	// ---- rtti.cpp : read-only RTTI scan of a loaded legacy module ----
	// Reports plugin classes that derive from game interfaces whose vtable changed in 1.7.99.
	// Returns the number of affected classes.
	int scan_rtti(HMODULE mod, const std::string& name);

	// ---- faultlog.cpp : first-chance exception attribution (log only, never handles) ----
	void fault_log_install(const std::vector<IdEntry>& installed);
	void fault_log_add_module(HMODULE mod, const std::string& name);

	// ---- skseproxy.cpp : SKSE interfaces handed to hosted plugins ----
	// Messaging fan-out (every hosted plugin gets every message) and co-save collision warnings.
	void skse_proxy_init(const SKSEInterface* real, bool messagingFanout);
	void skse_proxy_add_module(HMODULE mod, const std::string& name);
	void* (*skse_proxy_query())(std::uint32_t);  // QueryInterface for hosted plugins; null before init
	// Hosted plugin names (from SKSEPlugin_Version), declared before any is loaded.
	void skse_proxy_loading(const std::string& file);  // "" when SKSEPlugin_Load returned
	void skse_proxy_declare(const std::string& file, const std::string& pluginName, std::uint32_t version);
	const void* (*skse_proxy_plugin_info())(const char*);  // GetPluginInfo for hosted plugins

	// ---- interpose.cpp : per-module import-table interposer ----
	struct InterposeConfig
	{
		bool    spoofVersion{ true };
		Version spoof{};
		HANDLE  mapping{ nullptr };  // private table, may be null
		std::size_t mappingBytes{ 0 };
	};

	struct InterposeResult
	{
		int getFileVersionInfoW{ 0 };
		int verQueryValueW{ 0 };
		int getFileVersionInfoA{ 0 };
		int verQueryValueA{ 0 };
		int openFileMappingW{ 0 };
		int createFileMappingW{ 0 };
	};

	InterposeResult install_interposer(HMODULE legacy, const std::wstring& name, const InterposeConfig& cfg);
	bool            imports_bound(HMODULE mod);

	// ---- earlyload.cpp : patch a legacy module before its DllMain / static initializers run ----
	struct EarlyResult
	{
		bool            available{ false };  // loader notifications could be registered
		bool            sawModule{ false };   // notification arrived for the module
		bool            bound{ false };       // its imports were already bound at that point
		bool            patched{ false };     // interposer installed from the notification
		InterposeResult r;
	};
	// Arm before LoadLibraryW, disarm right after it returns.
	void        early_arm(const std::wstring& path, const std::wstring& name, const InterposeConfig& cfg, bool translate, bool faultLog);
	EarlyResult early_disarm();
}
