// Address Library readers and the private ID table handed to legacy plugins.
//
// Finding (2026-11-12, real files): 1.7.99 keeps the 1.6.1170 ID numbering. 392,696 of the 428,461
// 1.6.1170 IDs exist in versionlib-1-7-99-0.bin; 97.6% of neighbouring pairs keep an identical gap.
// New IDs continue from 522,615. So the installed library is the translation; no ID->ID remap is
// needed except for the 35,765 IDs 1.7.99 dropped, which only an overlay can address.
//
// Format 1/2 header and packed stream: as read by CommonLibSSE-NG 539d4ce (MIT),
//   include/REL/ID.h:256-282 (header), :335-412 (unpack).  [Verified against that source]
// Format 5 header and dense array: as read by CommonLibSSE-NG 9955ae3,
//   include/REL/IDDB.h:222-251, src/REL/IDDB.cpp:156-164, 274-301.  [Verified against that source]
//   The layout facts were re-implemented here from that description; no code was copied.
//   Meaning of pointerSize/dataFormat in format 5: [Unknown]; they are logged, not used.
//
// The table is built at runtime from the Address Library file the user installed.
// Nothing from that file is embedded in, or shipped with, this plugin.
#include "common.h"

#include <algorithm>

namespace bridge
{
	namespace
	{
		struct Cursor
		{
			const std::vector<std::uint8_t>& d;
			std::size_t                      pos{ 0 };
			bool                             ok{ true };

			template <class T>
			T get()
			{
				T v{};
				if (!ok || pos > d.size() || sizeof(T) > d.size() - pos) {
					ok = false;
					return v;
				}
				std::memcpy(&v, d.data() + pos, sizeof(T));
				pos += sizeof(T);
				return v;
			}

			void skip(std::size_t n)
			{
				if (!ok || pos > d.size() || n > d.size() - pos) {
					ok = false;
					return;
				}
				pos += n;
			}
		};

		bool parse_header(Cursor& c, AlHeader& h, std::string& err)
		{
			h = {};
			h.format = c.get<std::int32_t>();
			if (h.format == 1 || h.format == 2) {
				for (auto& part : h.version.v) {
					part = static_cast<std::uint16_t>(c.get<std::int32_t>());
				}
				const auto nameLen = c.get<std::int32_t>();
				if (nameLen < 0 || nameLen > 4096) {
					err = "implausible module name length in header";
					return false;
				}
				c.skip(static_cast<std::size_t>(nameLen));
				h.pointerSize = c.get<std::int32_t>();
				h.count = c.get<std::int32_t>();
			} else if (h.format == 5) {
				for (auto& part : h.version.v) {
					part = static_cast<std::uint16_t>(c.get<std::uint32_t>());
				}
				c.skip(64);  // char name[64]
				h.pointerSize = c.get<std::int32_t>();
				h.dataFormat = c.get<std::int32_t>();
				h.count = c.get<std::int32_t>();
			} else {
				err = "unsupported Address Library format " + std::to_string(h.format);
				return false;
			}
			if (!c.ok || h.count < 0) {
				err = "truncated or invalid header";
				return false;
			}
			h.dataOffset = c.pos;
			return true;
		}

		bool unpack_v2(Cursor& c, const AlHeader& h, std::vector<IdEntry>& out, std::string& err)
		{
			if (h.pointerSize <= 0) {
				err = "invalid pointer size in header";
				return false;
			}
			const auto    ptr = static_cast<std::uint64_t>(h.pointerSize);
			std::uint64_t id = 0, offset = 0, prevID = 0, prevOffset = 0;
			out.reserve(static_cast<std::size_t>(h.count));
			for (std::int32_t i = 0; i < h.count; ++i) {
				const auto type = c.get<std::uint8_t>();
				const auto lo = static_cast<std::uint8_t>(type & 0xF);
				const auto hi = static_cast<std::uint8_t>(type >> 4);
				switch (lo) {
				case 0: id = c.get<std::uint64_t>(); break;
				case 1: id = prevID + 1; break;
				case 2: id = prevID + c.get<std::uint8_t>(); break;
				case 3: id = prevID - c.get<std::uint8_t>(); break;
				case 4: id = prevID + c.get<std::uint16_t>(); break;
				case 5: id = prevID - c.get<std::uint16_t>(); break;
				case 6: id = c.get<std::uint16_t>(); break;
				case 7: id = c.get<std::uint32_t>(); break;
				default:
					err = "unhandled id type in packed stream";
					return false;
				}
				const std::uint64_t tmp = (hi & 8) != 0 ? (prevOffset / ptr) : prevOffset;
				switch (hi & 7) {
				case 0: offset = c.get<std::uint64_t>(); break;
				case 1: offset = tmp + 1; break;
				case 2: offset = tmp + c.get<std::uint8_t>(); break;
				case 3: offset = tmp - c.get<std::uint8_t>(); break;
				case 4: offset = tmp + c.get<std::uint16_t>(); break;
				case 5: offset = tmp - c.get<std::uint16_t>(); break;
				case 6: offset = c.get<std::uint16_t>(); break;
				case 7: offset = c.get<std::uint32_t>(); break;
				}
				if ((hi & 8) != 0) {
					offset *= ptr;
				}
				if (!c.ok) {
					err = "packed stream ends early at row " + std::to_string(i);
					return false;
				}
				out.push_back({ id, offset });
				prevID = id;
				prevOffset = offset;
			}
			return true;
		}

		bool unpack_v5(Cursor& c, const AlHeader& h, std::vector<IdEntry>& out, std::string& err)
		{
			const auto n = static_cast<std::size_t>(h.count);
			if (c.pos > c.d.size() || n > (c.d.size() - c.pos) / sizeof(std::uint32_t)) {
				err = "offset count exceeds file size";
				return false;
			}
			for (std::size_t id = 0; id < n; ++id) {
				std::uint32_t off = 0;
				std::memcpy(&off, c.d.data() + c.pos + id * sizeof(std::uint32_t), sizeof(off));
				if (off != 0) {  // 0 = ID absent (NG 9955ae3 IDDB.cpp:159)
					out.push_back({ id, off });
				}
			}
			return true;
		}
	}

	bool read_al_header(const std::wstring& path, AlHeader& hdr, std::string& err)
	{
		// Header only: read the first bytes, not the whole file.
		const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			err = "cannot open (GetLastError=" + std::to_string(GetLastError()) + ")";
			return false;
		}
		std::vector<std::uint8_t> buf(8192);
		DWORD                     got = 0;
		const BOOL                ok = ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr);
		CloseHandle(h);
		if (!ok) {
			err = "read failed";
			return false;
		}
		buf.resize(got);
		Cursor c{ buf };
		return parse_header(c, hdr, err);
	}

	bool load_al(const std::wstring& path, AlHeader& hdr, std::vector<IdEntry>& out, std::string& err)
	{
		out.clear();
		std::vector<std::uint8_t> buf;
		if (!read_file(path, buf, err)) {
			return false;
		}
		Cursor c{ buf };
		if (!parse_header(c, hdr, err)) {
			return false;
		}
		const bool ok = hdr.format == 5 ? unpack_v5(c, hdr, out, err) : unpack_v2(c, hdr, out, err);
		if (!ok) {
			out.clear();
			return false;
		}
		std::sort(out.begin(), out.end(), [](const IdEntry& a, const IdEntry& b) { return a.id < b.id; });
		return true;
	}

	bool read_overlay(const std::wstring& path, std::vector<std::pair<std::uint64_t, std::uint64_t>>& rules)
	{
		rules.clear();
		if (!file_exists(path)) {
			return false;
		}
		std::vector<std::uint8_t> buf;
		std::string               err;
		if (!read_file(path, buf, err)) {
			logf("  overlay: cannot read %s: %s", narrow(path).c_str(), err.c_str());
			return false;
		}
		std::string text(buf.begin(), buf.end());
		std::size_t pos = 0;
		int         lineNo = 0;
		while (pos <= text.size()) {
			const auto  eol = text.find('\n', pos);
			std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
			pos = eol == std::string::npos ? text.size() + 1 : eol + 1;
			++lineNo;
			if (const auto hash = line.find_first_of("#;"); hash != std::string::npos) {
				line.resize(hash);
			}
			if (line.find_first_not_of(" \t\r") == std::string::npos) {
				continue;
			}
			unsigned long long legacy = 0, target = 0;
			char               tail = 0;
			if (std::sscanf(line.c_str(), " %llu = %llu %c", &legacy, &target, &tail) != 2) {
				logf("  overlay: line %d ignored (expected \"legacyID = newID\")", lineNo);
				continue;
			}
			rules.emplace_back(legacy, target);
		}
		return true;
	}

	std::vector<IdEntry> build_legacy_table(const std::vector<IdEntry>& legacy, const std::vector<IdEntry>& current,
		const std::vector<std::pair<std::uint64_t, std::uint64_t>>& rules, TableStats& stats)
	{
		const auto find = [](const std::vector<IdEntry>& v, std::uint64_t id) -> const IdEntry* {
			const auto it = std::lower_bound(v.begin(), v.end(), id, [](const IdEntry& e, std::uint64_t x) { return e.id < x; });
			return it != v.end() && it->id == id ? &*it : nullptr;
		};

		stats = {};
		std::vector<IdEntry> table;
		table.reserve(legacy.size());
		for (const auto& e : legacy) {
			const IdEntry* now = find(current, e.id);
			table.push_back({ e.id, now ? now->offset : (kMissingOffset | e.id) });
			now ? ++stats.translated : ++stats.missing;
		}
		for (const auto& [from, to] : rules) {
			auto it = std::lower_bound(table.begin(), table.end(), from, [](const IdEntry& e, std::uint64_t x) { return e.id < x; });
			if (it == table.end() || it->id != from) {
				logf("  overlay: legacy ID %llu is not in the legacy library; rule skipped", static_cast<unsigned long long>(from));
				continue;
			}
			const IdEntry* target = find(current, to);
			if (!target) {
				logf("  overlay: new ID %llu is not in the installed library; rule skipped", static_cast<unsigned long long>(to));
				continue;
			}
			if ((it->offset & kMissingOffset) == kMissingOffset) {
				--stats.missing;
				++stats.translated;
			}
			it->offset = target->offset;
			++stats.overlaid;
		}
		stats.rows = table.size();
		return table;
	}

	// Source: CommonLibSSE-NG 9955ae3 (AE_CHECK(SKSE::RUNTIME_SSE_1_7_99, old, new) call sites). [Verified]
	// Checked against Address Library v13 files: each old ID exists in 1.6.1170, is absent from 1.7.99 and
	// 1.7.104, and each new ID exists in both. Same function signature on both sides of AE_CHECK.
	RemapRules known_remaps(const Version& legacy, const Version& game)
	{
		static constexpr Version k1799{ { 1, 7, 99, 0 } };
		if (legacy.v[0] != 1 || legacy.v[1] != 6 || game.less(k1799)) {
			return {};
		}
		return {
			{ 47286, 523983 },  // CombatUtilities GetAngleToProjectedTarget  (include/RE/C/CombatUtilities.h:15)
			{ 82331, 524270 },  // BSScaleformManager::IsValidName            (src/RE/B/BSScaleformManager.cpp:21)
			{ 77235, 524254 },  // BSGraphics::Renderer::RequestWindowResize  (src/RE/R/Renderer.cpp:112)
			{ 13358, 523463 },  // TES::GetWaterHeight                        (src/RE/T/TES.cpp:209)
		};
	}

	HANDLE make_mapping(const std::vector<IdEntry>& table)
	{
		const std::uint64_t bytes = static_cast<std::uint64_t>(table.size()) * sizeof(IdEntry);
		if (!bytes) {
			return nullptr;
		}
		// Unnamed: this mapping is private to the host and is never visible under the
		// shared CommonLibSSEOffsets-v2-* name.
		const HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
			static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes & 0xFFFFFFFFu), nullptr);
		if (!map) {
			return nullptr;
		}
		void* view = MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(bytes));
		if (!view) {
			CloseHandle(map);
			return nullptr;
		}
		std::memcpy(view, table.data(), static_cast<std::size_t>(bytes));
		UnmapViewOfFile(view);
		return map;
	}
}
