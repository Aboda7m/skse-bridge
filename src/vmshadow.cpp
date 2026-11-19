// Shadow SkyrimVM (EXPERIMENTAL, opt-in per plugin).
//
// Problem [Verified]: 1.7.99 inserted two event sinks at SkyrimVM+0x180, so every member from 0x180 on moved by
// +0x10 (allocation 0x8978 in 1.6.1170, 0x8988 in 1.7.104; CommonLibSSE-NG 16.9.0 SkyrimVM.h). A plugin built with
// the other layout reads the wrong members, most visibly the VM pointer (0x200 on 1.6, 0x210 on 1.7) used by
// VirtualMachine::GetSingleton() and by CommonLib's Papyrus argument/return conversion.
//
// What this does: the plugin gets the SkyrimVM singleton through an Address Library ID (400475 in every 1.6/1.7
// library) that the bridge answers. For a shadowed plugin that ID points at a bridge-owned global G. Reading G
// yields a pointer S to a shadow block that is never readable. Every access to S faults; the handler maps the
// plugin-layout offset to this runtime's offset, moves the instruction's base register by the difference, lets the
// plugin's own instruction run once against the real object (single step), and puts the register back. Atomic
// instructions stay atomic; nothing is emulated. G itself is also guarded so that it reads as null until the game
// has created its VM, exactly like the real global.
//
// Limits: only pointers that come from that ID are covered. A SkyrimVM* the game passes in (hook arguments) is the
// real object and is not translated. If game code touches S (a plugin passed it on), the access is mapped with the
// plugin's layout and logged as such. Instructions whose address the decoder cannot resolve are logged and left to
// fault (no guessing).
#include "common.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>

namespace bridge
{
	namespace
	{
		constexpr std::uint64_t kVmSingletonId = 400475;  // RELOCATION_ID(514315, 400475): SkyrimVM** (CommonLibSSE-NG, commonlib-shared)
		constexpr std::uint64_t kInsertAt = 0x180;
		constexpr std::uint64_t kInserted = 0x10;
		constexpr std::size_t   kSize16 = 0x8978;
		constexpr std::size_t   kSize17 = 0x8988;
		constexpr std::size_t   kPage = 0x1000;

		struct Region
		{
			std::uint8_t* base{ nullptr };  // page 0: C (readable cell holding S), page 1: G, then S, then dummy page
			std::uint8_t* cell{ nullptr };
			std::uint8_t* g{ nullptr };
			std::uint8_t* s{ nullptr };
			std::uint8_t* dummy{ nullptr };
			std::size_t   sBytes{ 0 };
			std::size_t   total{ 0 };
		} g_r;

		struct ModLayout
		{
			std::uintptr_t begin, end;
			std::string    name;
			bool           layout17;
		};

		SRWLOCK                 g_lock = SRWLOCK_INIT;
		std::vector<ModLayout>  g_mods;
		std::uintptr_t          g_realGlobal = 0;  // exe + offset of ID 400475 in the game's library
		bool                    g_game17 = false;
		PVOID                   g_veh = nullptr;
		bool                    g_ready = false;
		volatile LONG64         g_faults = 0, g_unresolved = 0, g_gameSide = 0, g_gReads = 0;

		struct Stat
		{
			unsigned long long count{ 0 };
			std::uint64_t      realOff{ 0 };
			bool               write{ false };
			std::uintptr_t     firstRip{ 0 };
		};
		std::map<std::pair<std::string, std::uint64_t>, Stat> g_stats;  // (module, plugin offset)
		unsigned                                              g_logged = 0;

		struct Pending
		{
			bool          active{ false };
			int           reg{ -1 };
			DWORD64       orig{ 0 };
			DWORD64       expect{ 0 };
		};
		thread_local Pending t_pending;
		thread_local int     t_lastLayout = -1;  // 0 = 1.6 layout, 1 = 1.7 layout

		DWORD64* reg_ptr(CONTEXT* c, int r)
		{
			switch (r) {
			case 0: return &c->Rax;
			case 1: return &c->Rcx;
			case 2: return &c->Rdx;
			case 3: return &c->Rbx;
			case 4: return &c->Rsp;
			case 5: return &c->Rbp;
			case 6: return &c->Rsi;
			case 7: return &c->Rdi;
			case 8: return &c->R8;
			case 9: return &c->R9;
			case 10: return &c->R10;
			case 11: return &c->R11;
			case 12: return &c->R12;
			case 13: return &c->R13;
			case 14: return &c->R14;
			case 15: return &c->R15;
			default: return nullptr;
			}
		}

		// Registers that form the memory operand of the instruction at `ip`. Covers legacy prefixes, REX, VEX (C4/C5),
		// 1-byte, 0F, 0F38 and 0F3A opcodes with a ModRM operand, and string instructions (RSI/RDI). Returns false for
		// forms without a register base (RIP-relative, absolute) or that it does not recognise.
		bool operand_regs(const std::uint8_t* ip, int& base, int& index, int& scale)
		{
			base = index = -1;
			scale = 1;
			const std::uint8_t* p = ip;
			for (int i = 0; i < 14; ++i, ++p) {
				const std::uint8_t b = *p;
				if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65 || b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3) {
					continue;
				}
				break;
			}
			std::uint8_t rex = 0;
			if ((*p & 0xF0) == 0x40) {
				rex = *p++;
			}
			bool rexB = rex & 1, rexX = rex & 2;
			if (*p == 0xC5) {  // 2-byte VEX: no X/B extension
				p += 2;
				rexB = rexX = false;
				++p;  // opcode
			} else if (*p == 0xC4) {
				const std::uint8_t b1 = p[1];
				rexX = !(b1 & 0x40);
				rexB = !(b1 & 0x20);
				p += 3;
				++p;  // opcode
			} else if (*p == 0x0F) {
				++p;
				if (*p == 0x38 || *p == 0x3A) {
					++p;
				}
				++p;
			} else {
				const std::uint8_t op = *p++;
				if (op >= 0xA4 && op <= 0xAF) {  // movs/cmps/stos/lods/scas: RSI and/or RDI
					base = 6;
					index = 7;
					return true;
				}
				if (op >= 0xA0 && op <= 0xA3) {
					return false;  // moffs
				}
			}
			const std::uint8_t modrm = *p++;
			const int          mod = modrm >> 6, rm = modrm & 7;
			if (mod == 3) {
				return false;
			}
			if (rm == 4) {
				const std::uint8_t sib = *p++;
				const int          sbase = sib & 7, sidx = (sib >> 3) & 7;
				scale = 1 << (sib >> 6);
				index = (sidx | (rexX ? 8 : 0)) == 4 ? -1 : (sidx | (rexX ? 8 : 0));
				base = (sbase == 5 && mod == 0) ? -1 : (sbase | (rexB ? 8 : 0));
				return base >= 0 || index >= 0;
			}
			if (rm == 5 && mod == 0) {
				return false;  // RIP-relative
			}
			base = rm | (rexB ? 8 : 0);
			return true;
		}

		const ModLayout* mod_for(std::uintptr_t ip)
		{
			for (const auto& m : g_mods) {
				if (ip >= m.begin && ip < m.end) {
					return &m;
				}
			}
			return nullptr;
		}

		// Plugin-layout offset -> this runtime's offset; false when the member does not exist here.
		bool map_offset(std::uint64_t o, bool plugin17, std::uint64_t& out)
		{
			if (plugin17 == g_game17 || o < kInsertAt) {
				out = o;
				return true;
			}
			if (!plugin17) {  // 1.6 plugin on 1.7: everything from 0x180 moved up
				out = o + kInserted;
				return true;
			}
			if (o < kInsertAt + kInserted) {  // 1.7 plugin on 1.6: the inserted sinks do not exist
				return false;
			}
			out = o - kInserted;
			return true;
		}

		bool rebase(CONTEXT* c, std::uintptr_t fault, std::uintptr_t target)
		{
			int base, index, scale;
			if (!operand_regs(reinterpret_cast<const std::uint8_t*>(c->Rip), base, index, scale)) {
				return false;
			}
			const auto near_fault = [&](int r) {
				if (r < 0) {
					return false;
				}
				const DWORD64 v = *reg_ptr(c, r);
				return v + 0x100000 >= fault && v <= fault + 0x100000;
			};
			int reg = near_fault(base) ? base : (scale == 1 && near_fault(index) ? index : -1);
			if (reg < 0 || reg == 4) {
				return false;
			}
			DWORD64* rp = reg_ptr(c, reg);
			t_pending = { true, reg, *rp, *rp + (target - fault) };
			*rp = t_pending.expect;
			c->EFlags |= 0x100;  // trap after this one instruction
			return true;
		}

		LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep)
		{
			const auto* er = ep->ExceptionRecord;
			CONTEXT*    c = ep->ContextRecord;
			if (er->ExceptionCode == EXCEPTION_SINGLE_STEP) {
				if (!t_pending.active) {
					return EXCEPTION_CONTINUE_SEARCH;
				}
				DWORD64* rp = reg_ptr(c, t_pending.reg);
				if (*rp == t_pending.expect) {
					*rp = t_pending.orig;  // the instruction did not overwrite the register: give back its value
				}
				t_pending.active = false;
				c->EFlags &= ~0x100ull;
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			const auto fault = static_cast<std::uintptr_t>(er->ExceptionInformation[1]);
			const bool write = er->ExceptionInformation[0] == 1;
			const auto rbase = reinterpret_cast<std::uintptr_t>(g_r.base);
			if (!g_ready || fault < rbase || fault >= rbase + g_r.total) {
				return EXCEPTION_CONTINUE_SEARCH;
			}

			// G: the singleton global as the plugin sees it.
			if (fault >= reinterpret_cast<std::uintptr_t>(g_r.g) && fault < reinterpret_cast<std::uintptr_t>(g_r.g) + kPage) {
				InterlockedIncrement64(&g_gReads);
				const bool live = *reinterpret_cast<void* volatile*>(g_realGlobal) != nullptr;
				const auto target = live ? reinterpret_cast<std::uintptr_t>(g_r.cell) + (fault - reinterpret_cast<std::uintptr_t>(g_r.g)) :
				                           g_realGlobal + (fault - reinterpret_cast<std::uintptr_t>(g_r.g));
				if (write || !rebase(c, fault, target)) {
					InterlockedIncrement64(&g_unresolved);
					logf("[vm-shadow] unresolved %s of the singleton global at RIP %p", write ? "write" : "read", reinterpret_cast<void*>(c->Rip));
					return EXCEPTION_CONTINUE_SEARCH;
				}
				return EXCEPTION_CONTINUE_EXECUTION;
			}

			const auto sb = reinterpret_cast<std::uintptr_t>(g_r.s);
			if (fault < sb || fault >= sb + g_r.sBytes) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			InterlockedIncrement64(&g_faults);
			const std::uint64_t o = fault - sb;
			AcquireSRWLockShared(&g_lock);
			const ModLayout* m = mod_for(static_cast<std::uintptr_t>(c->Rip));
			ReleaseSRWLockShared(&g_lock);
			bool plugin17;
			if (m) {
				plugin17 = m->layout17;
				t_lastLayout = plugin17 ? 1 : 0;
			} else {
				InterlockedIncrement64(&g_gameSide);
				plugin17 = t_lastLayout >= 0 ? t_lastLayout == 1 : !g_game17;
			}
			const auto real = *reinterpret_cast<std::uint8_t* volatile*>(g_realGlobal);
			std::uint64_t ro = 0;
			const bool    exists = map_offset(o, plugin17, ro);
			std::uintptr_t target;
			if (!real) {
				target = reinterpret_cast<std::uintptr_t>(g_r.dummy) + (o & (kPage - 1) & ~0xFull);
			} else if (!exists) {
				target = reinterpret_cast<std::uintptr_t>(g_r.dummy) + (o & 0xF);
			} else {
				target = reinterpret_cast<std::uintptr_t>(real) + ro;
			}

			{
				const std::string name = m ? m->name : std::string("<game code>");
				AcquireSRWLockExclusive(&g_lock);
				Stat& st = g_stats[{ name, o }];
				const bool first = st.count++ == 0;
				st.realOff = ro;
				st.write = st.write || write;
				if (first) {
					st.firstRip = static_cast<std::uintptr_t>(c->Rip);
				}
				const bool doLog = first && g_logged < 400;
				if (doLog) {
					++g_logged;
				}
				ReleaseSRWLockExclusive(&g_lock);
				if (doLog) {
					logf("[vm-shadow] %s %s SkyrimVM+0x%llX (%s layout) -> %s at RIP %p", name.c_str(), write ? "writes" : "reads",
						static_cast<unsigned long long>(o), plugin17 ? "1.7" : "1.6",
						!real ? "dummy (game has no VM yet)" :
						!exists ? "dummy (member does not exist in this runtime)" :
						("real +0x" + [&] { char b[24]; std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(ro)); return std::string(b); }()).c_str(),
						reinterpret_cast<void*>(c->Rip));
				}
			}
			if (!rebase(c, fault, target)) {
				InterlockedIncrement64(&g_unresolved);
				logf("[vm-shadow] UNRESOLVED instruction at RIP %p (bytes %02X %02X %02X %02X %02X %02X): left to fault", reinterpret_cast<void*>(c->Rip),
					reinterpret_cast<const std::uint8_t*>(c->Rip)[0], reinterpret_cast<const std::uint8_t*>(c->Rip)[1],
					reinterpret_cast<const std::uint8_t*>(c->Rip)[2], reinterpret_cast<const std::uint8_t*>(c->Rip)[3],
					reinterpret_cast<const std::uint8_t*>(c->Rip)[4], reinterpret_cast<const std::uint8_t*>(c->Rip)[5]);
				return EXCEPTION_CONTINUE_SEARCH;
			}
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		// Reserve near the exe so offsets from the image base fit in 32 bits (format-5 tables store u32 offsets).
		std::uint8_t* alloc_near_exe(std::size_t bytes)
		{
			const std::uintptr_t start = (exe_base() + exe_size() + 0xFFFF) & ~std::uintptr_t{ 0xFFFF };
			for (std::uintptr_t a = start; a < exe_base() + 0xF0000000ull; a += 0x10000) {
				if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
					return static_cast<std::uint8_t*>(p);
				}
			}
			return nullptr;
		}
	}

	bool vm_shadow_init(const Version& game, const std::vector<IdEntry>& gameLib)
	{
		if (g_ready) {
			return true;
		}
		const auto it = std::lower_bound(gameLib.begin(), gameLib.end(), kVmSingletonId, [](const IdEntry& e, std::uint64_t v) { return e.id < v; });
		if (it == gameLib.end() || it->id != kVmSingletonId) {
			logf("[vm-shadow] ID %llu not in this game's Address Library; shadow unavailable", static_cast<unsigned long long>(kVmSingletonId));
			return false;
		}
		g_game17 = game.at_least(1, 7, 99);
		g_realGlobal = exe_base() + it->offset;
		g_r.sBytes = (std::max(kSize16, kSize17) + kPage - 1) & ~(kPage - 1);
		g_r.total = kPage * 2 + g_r.sBytes + kPage;
		g_r.base = alloc_near_exe(g_r.total);
		if (!g_r.base) {
			logf("[vm-shadow] could not reserve memory near the game image; shadow unavailable");
			return false;
		}
		g_r.cell = g_r.base;
		g_r.g = g_r.base + kPage;
		g_r.s = g_r.base + 2 * kPage;
		g_r.dummy = g_r.s + g_r.sBytes;
		*reinterpret_cast<std::uint8_t**>(g_r.cell) = g_r.s;
		DWORD old = 0;
		VirtualProtect(g_r.g, kPage + g_r.sBytes, PAGE_NOACCESS, &old);
		g_veh = AddVectoredExceptionHandler(1, &on_exception);
		g_ready = g_veh != nullptr;
		logf("[vm-shadow] EXPERIMENTAL shadow SkyrimVM ready: game layout %s, global %p (real global SkyrimSE.exe+0x%llX), shadow %p (+0x%zX)",
			g_game17 ? "1.7" : "1.6", static_cast<void*>(g_r.g), static_cast<unsigned long long>(it->offset), static_cast<void*>(g_r.s), g_r.sBytes);
		return g_ready;
	}

	std::uint64_t vm_shadow_global_offset()
	{
		return g_ready ? reinterpret_cast<std::uintptr_t>(g_r.g) - exe_base() : 0;
	}

	std::uint64_t vm_singleton_id() { return kVmSingletonId; }

	void vm_shadow_add_module(HMODULE mod, const std::string& name, bool layout17)
	{
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const std::uint8_t*>(mod) + dos->e_lfanew);
		AcquireSRWLockExclusive(&g_lock);
		g_mods.push_back({ reinterpret_cast<std::uintptr_t>(mod), reinterpret_cast<std::uintptr_t>(mod) + nt->OptionalHeader.SizeOfImage, name, layout17 });
		ReleaseSRWLockExclusive(&g_lock);
		logf("[vm-shadow] %s uses the shadow with the %s SkyrimVM layout", name.c_str(), layout17 ? "1.7" : "1.6");
	}

	void vm_shadow_report(const char* when)
	{
		if (!g_ready) {
			return;
		}
		logf("[vm-shadow] summary (%s): %lld member access(es), %lld singleton read(s), %lld from game code, %lld unresolved", when,
			static_cast<long long>(g_faults), static_cast<long long>(g_gReads), static_cast<long long>(g_gameSide), static_cast<long long>(g_unresolved));
		AcquireSRWLockShared(&g_lock);
		for (const auto& [k, st] : g_stats) {
			logf("[vm-shadow]   %s SkyrimVM+0x%llX -> +0x%llX %s x%llu (first at %p)", k.first.c_str(), static_cast<unsigned long long>(k.second),
				static_cast<unsigned long long>(st.realOff), st.write ? "written" : "read", st.count, reinterpret_cast<void*>(st.firstRip));
		}
		ReleaseSRWLockShared(&g_lock);
	}

	// Static evidence: an immediate 400475 (the SkyrimVM singleton ID) in code, followed within 256 bytes by
	// mov r64,[r64+0x200] (1.6 layout) or [r64+0x210] (1.7 layout). Heuristic; the log shows each site.
	VmScan scan_vm_layout(const std::wstring& path)
	{
		VmScan out;
		std::vector<std::uint8_t> d;
		std::string               err;
		if (!read_file(path, d, err) || d.size() < 0x400) {
			return out;
		}
		const auto pe = *reinterpret_cast<const std::uint32_t*>(&d[0x3C]);
		if (pe + 0x108 > d.size()) {
			return out;
		}
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(&d[pe]);
		const auto* sec = IMAGE_FIRST_SECTION(nt);
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
			if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
				continue;
			}
			const std::size_t lo = sec[i].PointerToRawData, hi = std::min<std::size_t>(d.size(), lo + sec[i].SizeOfRawData);
			const std::uint8_t idb[4] = { 0x5B, 0x1C, 0x06, 0x00 };  // 400475
			for (std::size_t p = lo; p + 4 <= hi; ++p) {
				if (std::memcmp(&d[p], idb, 4) != 0) {
					continue;
				}
				out.idRefs++;
				for (std::size_t q = p + 4; q + 7 <= std::min(hi, p + 260); ++q) {
					const std::uint8_t rex = d[q], op = d[q + 1], modrm = d[q + 2];
					if ((rex & 0xF8) != 0x48 || op != 0x8B || (modrm >> 6) != 2 || (modrm & 7) == 4) {
						continue;
					}
					const auto disp = *reinterpret_cast<const std::uint32_t*>(&d[q + 3]);
					if (disp == 0x200 || disp == 0x210) {
						(disp == 0x210 ? out.votes17 : out.votes16)++;
						out.sites.push_back(static_cast<std::uint32_t>(q - lo + sec[i].VirtualAddress));
						break;
					}
				}
			}
		}
		out.layout = out.votes17 > out.votes16 ? 17 : (out.votes16 > out.votes17 ? 16 : 0);
		return out;
	}
}
