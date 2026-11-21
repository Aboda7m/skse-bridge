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
#include "trace.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <initializer_list>
#include <string>

namespace bridge
{
	namespace
	{
		constexpr std::size_t kPage = 0x1000;

		// One entry per shadowable class. Every member from insertAt moved by `inserted` bytes in 1.7.99.
		struct ClassDef
		{
			const char*   name;
			std::uint64_t id;        // Address Library ID of the global holding the object pointer (same in 1.6 and 1.7)
			std::uint64_t insertAt;
			std::uint64_t inserted;
			std::size_t   bytes;     // shadow span (>= object size in both layouts)
		};
		constexpr ClassDef kClasses[kShadowClassCount] = {
			// RELOCATION_ID(514315, 400475) SkyrimVM**; two amiibo sinks at 0x180 (alloc 0x8978 -> 0x8988) [Verified]
			{ "SkyrimVM", 400475, 0x180, 0x10, 0x9000 },
			// RELOCATION_ID(517014, 403521) NiPointer<PlayerCharacter>*; BSTEventSink<BSSystemEvent> at 0x2D8, everything
			// after +8 (event sources 0x2D8/0x330/0x388 -> 0x2E0/0x338/0x390, runtime data 0x3D8 -> 0x3E0) [NG 16.9.0]
			{ "PlayerCharacter", 403521, 0x2D8, 0x8, 0x2000 },
		};

		struct Region
		{
			std::uint8_t*  base{ nullptr };  // page 0: C (readable cell holding S), page 1: G, then S, then dummy page
			std::uint8_t*  cell{ nullptr };
			std::uint8_t*  g{ nullptr };
			std::uint8_t*  s{ nullptr };
			std::uint8_t*  dummy{ nullptr };
			std::size_t    sBytes{ 0 };
			std::size_t    total{ 0 };
			std::uintptr_t realGlobal{ 0 };  // exe + offset of the class ID in the game's library
			bool           ready{ false };
			int            pluginLayout{ -1 };  // layout of the plugins using this shadow (1 = 1.7), for game-side interior accesses
		} g_regions[kShadowClassCount];

		struct ModLayout
		{
			std::uintptr_t begin, end;
			std::string    name;
			bool           layout17;
			unsigned       classes;  // bit per ShadowClass
		};

		SRWLOCK                 g_lock = SRWLOCK_INIT;
		std::vector<ModLayout>  g_mods;
		bool                    g_game17 = false;
		PVOID                   g_veh = nullptr;
		bool                    g_ready = false;  // at least one class is ready
		volatile LONG64         g_faults = 0, g_unresolved = 0, g_gameSide = 0, g_gReads = 0;

		struct Stat
		{
			unsigned long long count{ 0 };
			std::uint64_t      realOff{ 0 };
			bool               write{ false };
			std::uintptr_t     firstRip{ 0 };
#if SKB_TRACE
			// TEST-ONLY (trace build): what the flow events need. Filled under g_lock; c/tr indexed [target][write].
			const char*        cls{ nullptr };
			std::string        plugin;  // empty = game code
			bool               layout17{ false };
			bool               whole{ false };
			unsigned long long c[3][2]{};
			unsigned long long tr[3][2]{};
			std::int64_t       lastQpc{ 0 };
#endif
		};
		std::map<std::pair<std::string, std::uint64_t>, Stat> g_stats;  // (class/module, plugin offset)
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
		bool map_offset(const ClassDef& k, std::uint64_t o, bool plugin17, std::uint64_t& out)
		{
			if (plugin17 == g_game17 || o < k.insertAt) {
				out = o;
				return true;
			}
			if (!plugin17) {  // 1.6 plugin on 1.7: everything from insertAt moved up
				out = o + k.inserted;
				return true;
			}
			if (o < k.insertAt + k.inserted) {  // 1.7 plugin on 1.6: the inserted member does not exist
				return false;
			}
			out = o - k.inserted;
			return true;
		}

		// The register that holds the pointer the instruction dereferences (base, else a scale-1 index), or -1.
		int pick_reg(CONTEXT* c, std::uintptr_t fault)
		{
			int base, index, scale;
			if (!operand_regs(reinterpret_cast<const std::uint8_t*>(c->Rip), base, index, scale)) {
				return -1;
			}
			const auto near_fault = [&](int r) {
				if (r < 0) {
					return false;
				}
				const DWORD64 v = *reg_ptr(c, r);
				return v + 0x100000 >= fault && v <= fault + 0x100000;
			};
			const int reg = near_fault(base) ? base : (scale == 1 && near_fault(index) ? index : -1);
			return reg == 4 ? -1 : reg;
		}

		bool rebase(CONTEXT* c, std::uintptr_t fault, std::uintptr_t target, int reg = -2)
		{
			if (reg == -2) {
				reg = pick_reg(c, fault);
			}
			if (reg < 0) {
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
			if (!g_ready) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			int cls = -1;
			for (int i = 0; i < kShadowClassCount; ++i) {
				const auto rb = reinterpret_cast<std::uintptr_t>(g_regions[i].base);
				if (g_regions[i].ready && fault >= rb && fault < rb + g_regions[i].total) {
					cls = i;
					break;
				}
			}
			if (cls < 0) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			Region&         r = g_regions[cls];
			const ClassDef& k = kClasses[cls];

			// G: the global as the plugin sees it.
			if (fault >= reinterpret_cast<std::uintptr_t>(r.g) && fault < reinterpret_cast<std::uintptr_t>(r.g) + kPage) {
				InterlockedIncrement64(&g_gReads);
				const bool live = *reinterpret_cast<void* volatile*>(r.realGlobal) != nullptr;
				const auto target = live ? reinterpret_cast<std::uintptr_t>(r.cell) + (fault - reinterpret_cast<std::uintptr_t>(r.g)) :
				                           r.realGlobal + (fault - reinterpret_cast<std::uintptr_t>(r.g));
				if (write || !rebase(c, fault, target)) {
					InterlockedIncrement64(&g_unresolved);
					logf("[shadow] unresolved %s of the %s global at RIP %p", write ? "write" : "read", k.name, reinterpret_cast<void*>(c->Rip));
					return EXCEPTION_CONTINUE_SEARCH;
				}
				return EXCEPTION_CONTINUE_EXECUTION;
			}

			const auto sb = reinterpret_cast<std::uintptr_t>(r.s);
			if (fault < sb || fault >= sb + r.sBytes) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			InterlockedIncrement64(&g_faults);
			const std::uint64_t o = fault - sb;
			const int           reg = pick_reg(c, fault);
			AcquireSRWLockShared(&g_lock);
			const ModLayout* m = mod_for(static_cast<std::uintptr_t>(c->Rip));
			ReleaseSRWLockShared(&g_lock);
			bool plugin17;
			bool gameWhole = false;
			if (m) {
				plugin17 = m->layout17;
				t_lastLayout = plugin17 ? 1 : 0;
			} else {
				InterlockedIncrement64(&g_gameSide);
				// Game code on the whole object (this == S): it uses its own layout. On an interior pointer the plugin
				// computed (an event source, a lock): the plugin's layout.
				gameWhole = reg >= 0 && *reg_ptr(c, reg) == sb;
				plugin17 = gameWhole ? g_game17 : (r.pluginLayout >= 0 ? r.pluginLayout == 1 : (t_lastLayout >= 0 ? t_lastLayout == 1 : !g_game17));
			}
			const auto    real = *reinterpret_cast<std::uint8_t* volatile*>(r.realGlobal);
			std::uint64_t ro = 0;
			const bool    exists = map_offset(k, o, plugin17, ro);
			std::uintptr_t target;
			if (!real) {
				target = reinterpret_cast<std::uintptr_t>(r.dummy) + (o & (kPage - 1) & ~0xFull);
			} else if (!exists) {
				target = reinterpret_cast<std::uintptr_t>(r.dummy) + (o & 0xF);
			} else {
				target = reinterpret_cast<std::uintptr_t>(real) + ro;
			}

			{
				const std::string name = std::string(k.name) + " " + (m ? m->name : (gameWhole ? std::string("<game code, whole object>") : std::string("<game code>")));
				AcquireSRWLockExclusive(&g_lock);
				Stat& st = g_stats[{ name, o }];
				const bool first = st.count++ == 0;
				st.realOff = ro;
				st.write = st.write || write;
				if (first) {
					st.firstRip = static_cast<std::uintptr_t>(c->Rip);
				}
#if SKB_TRACE
				if (trace::want_flows()) {
					if (first) {
						st.cls = k.name;
						st.plugin = m ? m->name : std::string();
						st.layout17 = plugin17;
						st.whole = gameWhole;
					}
					LARGE_INTEGER q{};
					QueryPerformanceCounter(&q);
					st.lastQpc = q.QuadPart;
					++st.c[!real ? 1 : (!exists ? 2 : 0)][write ? 1 : 0];
				}
#endif
				const bool doLog = first && g_logged < 400;
				if (doLog) {
					++g_logged;
				}
				ReleaseSRWLockExclusive(&g_lock);
				if (doLog) {
					char ro_s[32];
					std::snprintf(ro_s, sizeof(ro_s), "real +0x%llX", static_cast<unsigned long long>(ro));
					logf("[shadow] %s %s %s+0x%llX (%s layout) -> %s at RIP %p", m ? m->name.c_str() : (gameWhole ? "<game code, whole object>" : "<game code>"),
						write ? "writes" : "reads", k.name, static_cast<unsigned long long>(o), plugin17 ? "1.7" : "1.6",
						!real ? "dummy (object not created yet)" : !exists ? "dummy (member does not exist in this runtime)" : ro_s,
						reinterpret_cast<void*>(c->Rip));
				}
			}
			if (!rebase(c, fault, target, reg)) {
				InterlockedIncrement64(&g_unresolved);
				logf("[shadow] UNRESOLVED instruction on %s at RIP %p (bytes %02X %02X %02X %02X %02X %02X): left to fault", k.name, reinterpret_cast<void*>(c->Rip),
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

	bool shadow_init(ShadowClass cls, const Version& game, const std::vector<IdEntry>& gameLib)
	{
		Region&         r = g_regions[cls];
		const ClassDef& k = kClasses[cls];
		if (r.ready) {
			return true;
		}
		const auto it = std::lower_bound(gameLib.begin(), gameLib.end(), k.id, [](const IdEntry& e, std::uint64_t v) { return e.id < v; });
		if (it == gameLib.end() || it->id != k.id) {
			logf("[shadow] %s: ID %llu not in this game's Address Library; shadow unavailable", k.name, static_cast<unsigned long long>(k.id));
			SKB_TRACE_DO(trace::emit("shadow_ready", {}, trace::Fields().s("class", k.name).b("ok", false).s("why", "ID not in this game's Address Library")));
			return false;
		}
		g_game17 = game.at_least(1, 7, 99);
		r.realGlobal = exe_base() + it->offset;
		r.sBytes = (k.bytes + kPage - 1) & ~(kPage - 1);
		r.total = kPage * 2 + r.sBytes + kPage;
		r.base = alloc_near_exe(r.total);
		if (!r.base) {
			logf("[shadow] %s: could not reserve memory near the game image; shadow unavailable", k.name);
			SKB_TRACE_DO(trace::emit("shadow_ready", {}, trace::Fields().s("class", k.name).b("ok", false).s("why", "no memory near the game image")));
			return false;
		}
		r.cell = r.base;
		r.g = r.base + kPage;
		r.s = r.base + 2 * kPage;
		r.dummy = r.s + r.sBytes;
		*reinterpret_cast<std::uint8_t**>(r.cell) = r.s;
		DWORD old = 0;
		VirtualProtect(r.g, kPage + r.sBytes, PAGE_NOACCESS, &old);
		if (!g_veh) {
			g_veh = AddVectoredExceptionHandler(1, &on_exception);
		}
		r.ready = g_veh != nullptr;
		g_ready = g_ready || r.ready;
		logf("[shadow] EXPERIMENTAL shadow %s ready: game layout %s, global %p (real global SkyrimSE.exe+0x%llX), shadow %p (+0x%zX)", k.name,
			g_game17 ? "1.7" : "1.6", static_cast<void*>(r.g), static_cast<unsigned long long>(it->offset), static_cast<void*>(r.s), r.sBytes);
		SKB_TRACE_DO(trace::emit("shadow_ready", {},
			trace::Fields().s("class", k.name).b("ok", r.ready).s("game_layout", g_game17 ? "1.7" : "1.6").x("real_global", it->offset).u("bytes", r.sBytes)));
		return r.ready;
	}

	std::uint64_t shadow_global_offset(ShadowClass cls)
	{
		return g_regions[cls].ready ? reinterpret_cast<std::uintptr_t>(g_regions[cls].g) - exe_base() : 0;
	}

	std::uint64_t shadow_id(ShadowClass cls) { return kClasses[cls].id; }
	const char*   shadow_name(ShadowClass cls) { return kClasses[cls].name; }

	void shadow_add_module(HMODULE mod, const std::string& name, bool layout17, unsigned classes)
	{
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const std::uint8_t*>(mod) + dos->e_lfanew);
		AcquireSRWLockExclusive(&g_lock);
		g_mods.push_back({ reinterpret_cast<std::uintptr_t>(mod), reinterpret_cast<std::uintptr_t>(mod) + nt->OptionalHeader.SizeOfImage, name, layout17, classes });
		for (int i = 0; i < kShadowClassCount; ++i) {
			if (classes & (1u << i)) {
				g_regions[i].pluginLayout = layout17 ? 1 : 0;
			}
		}
		ReleaseSRWLockExclusive(&g_lock);
		logf("[shadow] %s uses the shadow(s)%s%s with the %s layout", name.c_str(), (classes & 1) ? " SkyrimVM" : "", (classes & 2) ? " PlayerCharacter" : "",
			layout17 ? "1.7" : "1.6");
		SKB_TRACE_DO(trace::emit("shadow_attach", name,
			trace::Fields().s("classes", std::string((classes & 1) ? "SkyrimVM" : "") + ((classes & 1) && (classes & 2) ? "," : "") + ((classes & 2) ? "PlayerCharacter" : ""))
				.s("layout", layout17 ? "1.7" : "1.6")));
	}

#if SKB_TRACE
	// TEST-ONLY. Called by the trace writer thread: one "vm_access" event per (member, target, read/write) that gained
	// accesses since the last call. n = total so far, d = new since the previous event for that key.
	void vm_trace_flush()
	{
		if (!g_ready || !trace::want_flows()) {
			return;
		}
		struct Ev
		{
			std::string        cls, plugin;
			bool               layout17, whole, write;
			int                target;
			std::uint64_t      off, real;
			unsigned long long n, d;
			std::int64_t       qpc;
		};
		std::vector<Ev> out;
		AcquireSRWLockExclusive(&g_lock);
		for (auto& [key, st] : g_stats) {
			for (int tg = 0; tg < 3; ++tg) {
				for (int w = 0; w < 2; ++w) {
					if (st.c[tg][w] != st.tr[tg][w]) {
						out.push_back({ st.cls ? st.cls : "", st.plugin, st.layout17, st.whole, w == 1, tg, key.second, st.realOff, st.c[tg][w],
							st.c[tg][w] - st.tr[tg][w], st.lastQpc });
						st.tr[tg][w] = st.c[tg][w];
					}
				}
			}
		}
		ReleaseSRWLockExclusive(&g_lock);
		static const char* const kTarget[3] = { "real", "pending", "absent" };  // pending: game object not created yet; absent: member does not exist in this runtime
		for (const auto& e : out) {
			trace::emit_at(trace::ms_from_qpc(e.qpc), "vm_access", e.plugin,
				trace::Fields().s("class", e.cls).b("game", e.plugin.empty()).b("whole", e.whole).x("off", e.off).x("real", e.real).b("write", e.write)
					.s("layout", e.layout17 ? "1.7" : "1.6").s("target", kTarget[e.target]).u("n", e.n).u("d", e.d));
		}
	}
#endif

	void vm_shadow_report(const char* when)
	{
		if (!g_ready) {
			return;
		}
		logf("[shadow] summary (%s): %lld member access(es), %lld global read(s), %lld from game code, %lld unresolved", when,
			static_cast<long long>(g_faults), static_cast<long long>(g_gReads), static_cast<long long>(g_gameSide), static_cast<long long>(g_unresolved));
		AcquireSRWLockShared(&g_lock);
		for (const auto& [key, st] : g_stats) {
			logf("[shadow]   %s +0x%llX -> +0x%llX %s x%llu (first at %p)", key.first.c_str(), static_cast<unsigned long long>(key.second),
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
		// PlayerCharacter: singleton ID 403521 in code, and RTTI of sinks for the event sources that moved in 1.7.99.
		const std::uint8_t pid[4] = { 0x41, 0x28, 0x06, 0x00 };  // 403521
		for (std::size_t p = 0; p + 4 <= d.size(); ++p) {
			if (std::memcmp(&d[p], pid, 4) == 0) {
				out.playerRefs++;
			}
		}
		const std::string text(d.begin(), d.end());
		for (const char* ev : { "BSTEventSink@UBGSActorCellEvent@RE@@", "BSTEventSink@UBGSActorDeathEvent@RE@@", "BSTEventSink@UPositionPlayerEvent@RE@@" }) {
			if (text.find(ev) != std::string::npos) {
				out.playerSinks.push_back(std::string(ev).substr(16, std::strlen(ev) - 16 - 5));
			}
		}
		return out;
	}
}
