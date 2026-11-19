// Patch audit and patch guard: what hosted plugins write into SkyrimSE.exe.
//
// Why: Address Library translation makes every ID point at the right place on the new runtime, but many
// plugins patch *inside* a function: "ID + 0x7B" where 0x7B was read off the 1.6.1170 machine code. If
// 1.7.x compiled that function differently, the same offset lands in the middle of another instruction.
// The plugin then overwrites game code that is not the call it meant to hook, and the game crashes later,
// in game code, with no plugin frame on the stack. No ID table can fix that; the bridge can only see it.
//
// How (no game code is hooked; only the hosted plugin's own import table, like interpose.cpp):
//   CommonLibSSE-NG REL::safe_write / safe_fill (src/REL/Relocation.cpp:7-30, verified in lib/), the
//   trampoline and MinHook all call VirtualProtect(target, n, PAGE_EXECUTE_READWRITE) before writing and
//   VirtualProtect(target, n, old) after. The interposer sees both calls, so the bridge knows the address,
//   the size, the bytes before and the bytes after.
//
// Judgement for a write into executable code at "nearest ID + delta":
//   delta == 0                       function start (MinHook, whole-function replacement): ok
//   span of that ID unchanged        ok. The span is the distance to the next ID present in both Address
//                                    Libraries; equal spans are evidence (not proof) of unchanged code.
//   span changed, 5-byte write over E8/E9, or 6-byte write over FF 15/FF 25
//                                    the call/jump a hook replaces is there: allowed, warned
//   span changed, anything else      the bytes are not what a call hook replaces, or a byte patch into code
//                                    that changed: BLOCKED when patch_guard=1
// A vtable write is located from SkyrimSE.exe's own RTTI. A slot at or after an index where 1.7.99 inserted
// virtual functions (rtti.cpp kChanges) now belongs to a different function: BLOCKED when patch_guard=1.
//
// Blocking = the VirtualProtect call fails. CommonLibSSE-NG's safe_write then skips the memcpy (the assert
// is compiled out in release builds), so the feature behind that patch is missing instead of corrupting game
// code. Other libraries may react differently; every refusal is logged with the plugin name.
#include "common.h"

#include <algorithm>
#include <map>

namespace bridge
{
	namespace
	{
		struct Off
		{
			std::uint64_t offset, id;
			bool operator<(const Off& o) const { return offset < o.offset; }
		};

		struct Pending
		{
			std::uintptr_t addr;
			std::size_t    size;
			std::string    module;
			std::uint8_t   before[16];
			std::size_t    n;
			std::string    where;
			std::string    note;
		};

		struct Counts
		{
			unsigned ok{ 0 }, warned{ 0 }, blocked{ 0 }, notRestored{ 0 };
		};

		SRWLOCK                       g_lock = SRWLOCK_INIT;
		bool                          g_ready = false;
		bool                          g_guard = false;
		std::vector<Off>              g_cur;   // IDs present in both libraries, by current offset
		std::vector<IdEntry>          g_leg;   // legacy library, by ID
		std::vector<std::uint64_t>    g_legCommon;  // legacy offsets of the same IDs, sorted
		std::vector<Pending>          g_pending;
		std::map<std::string, Counts> g_counts;
		unsigned                      g_lines = 0;
		constexpr unsigned            kMaxLines = 4000;

		bool writable(DWORD p)
		{
			p &= 0xFF;
			return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
		}

		bool executable(std::uint64_t a)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (!a || !VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
				return false;
			}
			const DWORD p = mbi.Protect & 0xFF;
			return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
		}

		std::string hex(const std::uint8_t* b, std::size_t n)
		{
			std::string s;
			char        t[4];
			for (std::size_t i = 0; i < n; ++i) {
				std::snprintf(t, sizeof(t), i ? " %02X" : "%02X", b[i]);
				s += t;
			}
			return s;
		}

		struct Span
		{
			bool          found{ false };
			std::uint64_t id{ 0 }, delta{ 0 }, now{ 0 }, then{ 0 };
		};

		// Nearest ID at or below `rva` that exists in both libraries, and its span in each.
		Span span_of(std::uint64_t rva)
		{
			Span s;
			auto it = std::upper_bound(g_cur.begin(), g_cur.end(), Off{ rva, ~0ull });
			if (it == g_cur.begin()) {
				return s;
			}
			const auto cur = std::prev(it);
			if (rva - cur->offset >= 0x10000) {
				return s;
			}
			s.found = true;
			s.id = cur->id;
			s.delta = rva - cur->offset;
			s.now = it != g_cur.end() ? it->offset - cur->offset : 0;
			const auto le = std::lower_bound(g_leg.begin(), g_leg.end(), cur->id, [](const IdEntry& e, std::uint64_t v) { return e.id < v; });
			if (le != g_leg.end() && le->id == cur->id) {
				const auto nx = std::upper_bound(g_legCommon.begin(), g_legCommon.end(), le->offset);
				s.then = nx != g_legCommon.end() ? *nx - le->offset : 0;
			}
			return s;
		}

		enum class Kind
		{
			ok,
			warn,
			block
		};

		Kind judge(std::uintptr_t addr, std::size_t n, const std::uint8_t* b, std::size_t have, std::string& where, std::string& note)
		{
			const std::uintptr_t eb = exe_base();
			const std::uint64_t  rva = addr - eb;
			char                 buf[512];
			const Span          s = span_of(rva);
			if (s.found) {
				std::snprintf(buf, sizeof(buf), "SkyrimSE.exe+0x%llX (ID %llu+0x%llX)", static_cast<unsigned long long>(rva),
					static_cast<unsigned long long>(s.id), static_cast<unsigned long long>(s.delta));
			} else {
				std::snprintf(buf, sizeof(buf), "SkyrimSE.exe+0x%llX", static_cast<unsigned long long>(rva));
			}
			where = buf;

			if (!exe_rva_exec(rva)) {
				// Data. A vtable slot if an RTTI locator precedes a nearby 8-aligned address.
				// Walk back over code pointers only; the first address preceded by a valid RTTI locator is the vtable.
				for (std::uint64_t k = 0; addr % 8 == 0 && k < 512 && addr >= eb + 8 * k + 8; ++k) {
					const std::uint64_t entry = *reinterpret_cast<const std::uint64_t*>(addr - 8 * k);
					if (k > 0 && !executable(entry)) {
						break;  // not inside a vtable (slots already hooked by a plugin point at its executable code)
					}
					GameVtableInfo vi;
					if (!rtti_exe_vtable(addr - 8 * k, vi)) {
						continue;
					}
					const unsigned slot = static_cast<unsigned>(k);
					if (vi.changed && slot >= vi.insertAt) {
						const int was = slot >= vi.insertAt + vi.added ? static_cast<int>(slot - vi.added) : -1;
						std::snprintf(buf, sizeof(buf),
							"vtable %s slot %u. %s gained %u slot(s) at index %u in 1.7.99: slot %u now holds %s; a plugin built for "
							"1.6.1170 that patches slot %u meant the function that is now slot %u",
							vi.cls.c_str(), slot, vi.base.c_str(), vi.added, vi.insertAt, slot,
							was >= 0 ? ("what 1.6.1170 had at slot " + std::to_string(was)).c_str() : "a function that did not exist in 1.6.1170",
							slot, slot + vi.added);
						note = buf;
						return Kind::block;
					}
					std::snprintf(buf, sizeof(buf), "vtable %s slot %u%s", vi.cls.c_str(), slot, vi.changed ? " (before the inserted slots: unchanged)" : "");
					note = buf;
					return Kind::ok;
				}
				note = "data";
				return Kind::ok;
			}

			if (!s.found) {
				note = "code with no Address Library ID nearby; not judged";
				return Kind::warn;
			}
			if (s.delta == 0) {
				note = "function start";
				return Kind::ok;
			}
			if (s.now && s.then && s.now == s.then) {
				std::snprintf(buf, sizeof(buf), "in-function +0x%llX; ID span unchanged (0x%llX) since 1.6.1170, offset plausibly still valid",
					static_cast<unsigned long long>(s.delta), static_cast<unsigned long long>(s.now));
				note = buf;
				return Kind::ok;
			}
			const bool callSite = (n == 5 && have >= 1 && (b[0] == 0xE8 || b[0] == 0xE9)) ||
			                      (n == 6 && have >= 2 && b[0] == 0xFF && (b[1] == 0x15 || b[1] == 0x25));
			char spans[96];
			if (s.then) {
				std::snprintf(spans, sizeof(spans), "0x%llX in 1.6.1170, 0x%llX now", static_cast<unsigned long long>(s.then),
					static_cast<unsigned long long>(s.now));
			} else {
				std::snprintf(spans, sizeof(spans), "unknown in 1.6.1170, 0x%llX now", static_cast<unsigned long long>(s.now));
			}
			if (callSite) {
				std::snprintf(buf, sizeof(buf), "in-function +0x%llX of a function that changed size (%s), but the %s it replaces is there",
					static_cast<unsigned long long>(s.delta), spans, n == 5 ? "call/jmp rel32" : "call/jmp [rip+disp]");
				note = buf;
				return Kind::warn;
			}
			if (n == 5 || n == 6) {
				std::snprintf(buf, sizeof(buf),
					"in-function +0x%llX of a function that changed size (%s). A %zu-byte hook replaces a %s, but the game has %s here: "
					"the offset was taken from 1.6.1170 code and misses on this runtime",
					static_cast<unsigned long long>(s.delta), spans, n, n == 5 ? "E8/E9 rel32 call/jmp" : "FF 15/FF 25 indirect call/jmp",
					hex(b, std::min<std::size_t>(have, 8)).c_str());
			} else {
				std::snprintf(buf, sizeof(buf),
					"%zu-byte write at in-function +0x%llX of a function that changed size (%s); bytes chosen for 1.6.1170 code, unverifiable here",
					n, static_cast<unsigned long long>(s.delta), spans);
			}
			note = buf;
			return Kind::block;
		}

		void line(const char* fmt, ...)
		{
			if (g_lines >= kMaxLines) {
				return;
			}
			if (++g_lines == kMaxLines) {
				logf("[patch] log limit reached; further writes are counted only");
				return;
			}
			char    buf[1400];
			va_list ap;
			va_start(ap, fmt);
			std::vsnprintf(buf, sizeof(buf), fmt, ap);
			va_end(ap);
			logf("%s", buf);
		}
	}

	void patch_audit_init(const std::vector<IdEntry>& legacy, const std::vector<IdEntry>& current, bool guard)
	{
		AcquireSRWLockExclusive(&g_lock);
		g_leg = legacy;
		g_cur.clear();
		g_legCommon.clear();
		g_cur.reserve(current.size());
		g_legCommon.reserve(current.size());
		for (const auto& e : current) {
			const auto it = std::lower_bound(g_leg.begin(), g_leg.end(), e.id, [](const IdEntry& x, std::uint64_t v) { return x.id < v; });
			if (it != g_leg.end() && it->id == e.id) {
				g_cur.push_back({ e.offset, e.id });
				g_legCommon.push_back(it->offset);
			}
		}
		std::sort(g_cur.begin(), g_cur.end());
		std::sort(g_legCommon.begin(), g_legCommon.end());
		g_guard = guard;
		g_ready = !g_cur.empty();
		ReleaseSRWLockExclusive(&g_lock);
		logf("patch audit: %zu IDs present in both libraries; patch_guard=%d", g_cur.size(), guard ? 1 : 0);
	}

	bool patch_audit_protect(const std::string& module, void* p, std::size_t size, DWORD newProtect)
	{
		const auto addr = reinterpret_cast<std::uintptr_t>(p);
		const auto eb = exe_base();
		if (!g_ready || !p || addr < eb || addr - eb >= exe_size()) {
			return true;
		}
		AcquireSRWLockExclusive(&g_lock);
		bool allow = true;
		if (writable(newProtect)) {
			Pending pd{};
			pd.addr = addr;
			pd.size = size;
			pd.module = module;
			pd.n = std::min<std::size_t>({ size, sizeof(pd.before), static_cast<std::size_t>(exe_size() - (addr - eb)) });
			std::memcpy(pd.before, p, pd.n);
			const Kind k = judge(addr, size, pd.before, pd.n, pd.where, pd.note);
			auto&      c = g_counts[module];
			if (k == Kind::block && g_guard) {
				++c.blocked;
				line("[patch] %s: BLOCKED %zu-byte write at %s: %s. Refused so game code stays intact; the plugin feature behind this patch "
					 "will not work. (patch_guard=0 allows it.)",
					module.c_str(), size, pd.where.c_str(), pd.note.c_str());
				allow = false;
			} else {
				pd.note = (k == Kind::block ? "UNSAFE (patch_guard=0, allowed): " : (k == Kind::warn ? "WARNING: " : "")) + pd.note;
				(k == Kind::ok ? c.ok : c.warned)++;
				g_pending.push_back(std::move(pd));
			}
		} else {
			for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
				if (it->addr != addr) {
					continue;
				}
				std::uint8_t after[16]{};
				std::memcpy(after, p, it->n);
				line("[patch] %s: %zu bytes at %s: %s -> %s. %s", it->module.c_str(), it->size, it->where.c_str(), hex(it->before, it->n).c_str(),
					hex(after, it->n).c_str(), it->note.c_str());
				g_pending.erase(it);
				break;
			}
		}
		ReleaseSRWLockExclusive(&g_lock);
		return allow;
	}

	void patch_audit_after(void* p, DWORD newProtect, BOOL ok)
	{
		if (ok || !writable(newProtect)) {
			return;
		}
		AcquireSRWLockExclusive(&g_lock);
		const auto addr = reinterpret_cast<std::uintptr_t>(p);
		for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
			if (it->addr == addr) {
				g_pending.erase(it);
				break;
			}
		}
		ReleaseSRWLockExclusive(&g_lock);
	}

	void patch_audit_flush(const char* when)
	{
		if (!g_ready) {
			return;
		}
		AcquireSRWLockExclusive(&g_lock);
		for (const auto& pd : g_pending) {
			std::uint8_t now[16]{};
			std::memcpy(now, reinterpret_cast<const void*>(pd.addr), pd.n);
			++g_counts[pd.module].notRestored;
			line("[patch] %s: %zu bytes at %s made writable and not restored (yet): %s -> %s. %s", pd.module.c_str(), pd.size, pd.where.c_str(),
				hex(pd.before, pd.n).c_str(), hex(now, pd.n).c_str(), pd.note.c_str());
		}
		g_pending.clear();
		for (const auto& [m, c] : g_counts) {
			logf("[patch] summary %s (%s): %u ok, %u warned, %u blocked, %u left writable", m.c_str(), when, c.ok, c.warned, c.blocked, c.notRestored);
		}
		ReleaseSRWLockExclusive(&g_lock);
	}
}
