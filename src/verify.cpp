// Checks that translated Address Library offsets land where they should in the running SkyrimSE.exe.
//
// Facts used (PE format, no game knowledge):
//  - every offset must be inside the image;
//  - x64 non-leaf functions have a RUNTIME_FUNCTION entry in the exception directory (.pdata);
//    an Address Library function ID normally points at such a BeginAddress.
// A broken translation would scatter offsets: few would hit function starts. This is a plausibility
// check, not a proof that an ID still names the same function.
#include "common.h"

#include <algorithm>

namespace bridge
{
	namespace
	{
		struct Section
		{
			std::uint32_t rva{ 0 };
			std::uint32_t size{ 0 };
			bool          exec{ false };
			char          name[9]{};
		};

		struct ExeInfo
		{
			std::uintptr_t             base{ 0 };
			std::uint32_t              size{ 0 };
			std::vector<Section>       sections;
			std::vector<std::uint32_t> funcStarts;  // sorted RUNTIME_FUNCTION::BeginAddress
		};

		const ExeInfo& info()
		{
			static const ExeInfo e = [] {
				ExeInfo r;
				auto* const base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
				const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
				const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
				r.base = reinterpret_cast<std::uintptr_t>(base);
				r.size = nt->OptionalHeader.SizeOfImage;
				const auto* sec = IMAGE_FIRST_SECTION(nt);
				for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
					Section s;
					s.rva = sec[i].VirtualAddress;
					s.size = std::max(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData);
					s.exec = (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
					std::memcpy(s.name, sec[i].Name, 8);
					r.sections.push_back(s);
				}
				const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
				if (dir.VirtualAddress && dir.Size) {
					const auto* rf = reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress);
					const std::size_t n = dir.Size / sizeof(RUNTIME_FUNCTION);
					r.funcStarts.reserve(n);
					for (std::size_t i = 0; i < n; ++i) {
						r.funcStarts.push_back(rf[i].BeginAddress);
					}
					std::sort(r.funcStarts.begin(), r.funcStarts.end());
				}
				return r;
			}();
			return e;
		}

		const Section* section_of(std::uint64_t off)
		{
			for (const auto& s : info().sections) {
				if (off >= s.rva && off - s.rva < s.size) {
					return &s;
				}
			}
			return nullptr;
		}

		bool is_func_start(std::uint64_t off)
		{
			const auto& f = info().funcStarts;
			return off <= 0xFFFFFFFFull && std::binary_search(f.begin(), f.end(), static_cast<std::uint32_t>(off));
		}

		bool poisoned(std::uint64_t off) { return (off & 0xFFFF000000000000ull) == kMissingOffset; }

		const IdEntry* find(const std::vector<IdEntry>& v, std::uint64_t id)
		{
			const auto it = std::lower_bound(v.begin(), v.end(), id, [](const IdEntry& e, std::uint64_t x) { return e.id < x; });
			return it != v.end() && it->id == id ? &*it : nullptr;
		}

		std::string describe(std::uint64_t off)
		{
			char buf[96];
			const Section* s = section_of(off);
			if (!s) {
				std::snprintf(buf, sizeof(buf), "+0x%llX OUTSIDE IMAGE", static_cast<unsigned long long>(off));
			} else {
				std::snprintf(buf, sizeof(buf), "+0x%llX %s%s", static_cast<unsigned long long>(off), s->name,
					s->exec ? (is_func_start(off) ? " function-start" : " NOT-a-function-start") : "");
			}
			return buf;
		}
	}

	std::uintptr_t exe_base() { return info().base; }
	std::uint32_t  exe_size() { return info().size; }

	void verify_table(const std::vector<IdEntry>& table)
	{
		const auto& e = info();
		std::size_t live = 0, outside = 0, code = 0, starts = 0, data = 0, poison = 0;
		for (const auto& row : table) {
			if (poisoned(row.offset)) {
				++poison;
				continue;
			}
			++live;
			const Section* s = section_of(row.offset);
			if (!s) {
				++outside;
			} else if (s->exec) {
				++code;
				starts += is_func_start(row.offset);
			} else {
				++data;
			}
		}
		logf("verify: exe image 0x%llX size 0x%X, %zu sections, %zu .pdata function entries",
			static_cast<unsigned long long>(e.base), e.size, e.sections.size(), e.funcStarts.size());
		logf("verify: %zu resolved IDs: %zu outside the image, %zu in data sections, %zu in code of which %zu (%.1f%%) are exact function starts; %zu poisoned",
			live, outside, data, code, starts, code ? 100.0 * static_cast<double>(starts) / static_cast<double>(code) : 0.0, poison);
		if (outside) {
			logf("verify: WARNING: %zu IDs resolve outside SkyrimSE.exe. The installed Address Library does not match this exe.", outside);
		}
	}

	void check_ids(const std::wstring& path, const std::vector<IdEntry>& legacy, const std::vector<IdEntry>& table)
	{
		std::vector<std::uint8_t> buf;
		std::string               err;
		if (!file_exists(path)) {
			return;
		}
		if (!read_file(path, buf, err)) {
			logf("  check: cannot read %s: %s", narrow(path).c_str(), err.c_str());
			return;
		}
		std::vector<std::uint64_t> ids;
		std::uint64_t              cur = 0;
		bool                       in = false;
		for (std::size_t i = 0; i <= buf.size(); ++i) {
			const char c = i < buf.size() ? static_cast<char>(buf[i]) : ' ';
			if (c >= '0' && c <= '9') {
				cur = cur * 10 + static_cast<std::uint64_t>(c - '0');
				in = true;
			} else if (in) {
				ids.push_back(cur);
				cur = 0;
				in = false;
			}
		}
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		logf("  check: %zu IDs from %s", ids.size(), narrow(path).c_str());
		std::size_t bad = 0;
		for (const auto id : ids) {
			const IdEntry* old = find(legacy, id);
			const IdEntry* now = find(table, id);
			if (!old) {
				continue;  // not an Address Library ID of the legacy runtime (likely a constant in the text)
			}
			if (!now || poisoned(now->offset)) {
				++bad;
				logf("    ID %llu: MISSING in the installed library (poisoned; add a compat .ids rule if you know the new ID)",
					static_cast<unsigned long long>(id));
				continue;
			}
			const Section* s = section_of(now->offset);
			const bool     suspicious = !s || (s->exec && !is_func_start(now->offset));
			bad += suspicious;
			logf("    ID %llu: 1.6 +0x%llX -> now %s%s", static_cast<unsigned long long>(id),
				static_cast<unsigned long long>(old->offset), describe(now->offset).c_str(),
				suspicious ? "  <- check (mid-function IDs are normal only for hook sites)" : "");
		}
		logf("  check: %zu ID(s) need attention", bad);
	}
}
