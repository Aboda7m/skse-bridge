// First-chance exception attribution for hosted plugins.
//
// A vectored handler that only logs and always returns EXCEPTION_CONTINUE_SEARCH: it never handles,
// resumes or changes anything. It reports an exception when
//   - the faulting instruction is in a hosted legacy module, or
//   - a hosted module is on the faulting thread's stack (a game function called by the plugin faulted), or
//   - a register holds a poisoned Address Library address (a missing ID was used).
// Logged per event: code, location as module+RVA, SkyrimSE.exe addresses mapped back to the nearest
// Address Library ID (IDs are stable across 1.6.1170 -> 1.7.x, see docs/DESIGN.md), the stack, and for
// C++ exceptions the thrown type and std::exception::what().
// First-chance means the exception may still be caught later; that is exactly how a plugin can stop
// working while the game keeps running.
#include "common.h"

#include <algorithm>

namespace bridge
{
	namespace
	{
		struct Hosted
		{
			std::uintptr_t base, end;
			char           name[64];
		};

		constexpr std::size_t kMaxEvents = 64;
		constexpr unsigned    kMaxFrames = 24;

		SRWLOCK                                                   g_lock = SRWLOCK_INIT;
		std::vector<Hosted>                                       g_hosted;
		std::vector<std::pair<std::uint64_t, std::uint64_t>>      g_byOffset;  // (offset, id), sorted by offset
		volatile LONG                                             g_events = 0;
		std::pair<DWORD, std::uintptr_t>                          g_seen[kMaxEvents]{};
		PVOID                                                     g_handle = nullptr;
		thread_local bool                                         t_inside = false;

		bool readable(const void* p, std::size_t n)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
				return false;
			}
			const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
			return reinterpret_cast<std::uintptr_t>(p) + n <= end;
		}

		const Hosted* hosted_at(std::uintptr_t a)
		{
			for (const auto& h : g_hosted) {
				if (a >= h.base && a < h.end) {
					return &h;
				}
			}
			return nullptr;
		}

		std::string where(std::uintptr_t a)
		{
			char buf[160];
			if (const Hosted* h = hosted_at(a)) {
				std::snprintf(buf, sizeof(buf), "%s+0x%llX", h->name, static_cast<unsigned long long>(a - h->base));
				return buf;
			}
			const std::uintptr_t eb = exe_base();
			if (a >= eb && a - eb < exe_size()) {
				const std::uint64_t off = a - eb;
				auto it = std::upper_bound(g_byOffset.begin(), g_byOffset.end(), std::make_pair(off, ~0ull));
				if (it != g_byOffset.begin() && off - std::prev(it)->first < 0x10000) {
					--it;
					std::snprintf(buf, sizeof(buf), "SkyrimSE.exe+0x%llX (ID %llu+0x%llX)", static_cast<unsigned long long>(off),
						static_cast<unsigned long long>(it->second), static_cast<unsigned long long>(off - it->first));
				} else {
					std::snprintf(buf, sizeof(buf), "SkyrimSE.exe+0x%llX", static_cast<unsigned long long>(off));
				}
				return buf;
			}
			HMODULE mod = nullptr;
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(a), &mod) && mod) {
				wchar_t path[MAX_PATH]{};
				GetModuleFileNameW(mod, path, MAX_PATH);
				std::snprintf(buf, sizeof(buf), "%s+0x%llX", narrow(base_name(path)).c_str(),
					static_cast<unsigned long long>(a - reinterpret_cast<std::uintptr_t>(mod)));
				return buf;
			}
			std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(a));
			return buf;
		}

		unsigned unwind(const CONTEXT& start, std::uintptr_t* frames)
		{
			CONTEXT   c = start;
			ULONG_PTR lo = 0, hi = 0;
			GetCurrentThreadStackLimits(&lo, &hi);
			unsigned n = 0;
			while (n < kMaxFrames && c.Rip) {
				frames[n++] = c.Rip;
				if (c.Rsp < lo || c.Rsp + 8 > hi) {
					break;
				}
				DWORD64 imageBase = 0;
				auto*   rf = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
				if (!rf) {
					c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);  // leaf function
					c.Rsp += 8;
				} else {
					PVOID                          handlerData = nullptr;
					DWORD64                        establisher = 0;
					KNONVOLATILE_CONTEXT_POINTERS  nv{};
					RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, rf, &c, &handlerData, &establisher, &nv);
				}
				if (c.Rsp < lo || c.Rsp > hi) {
					break;
				}
			}
			return n;
		}

		// MSVC C++ exception: ExceptionInformation = { magic, object, ThrowInfo*, imageBase }.
		std::string cxx_info(const EXCEPTION_RECORD& er)
		{
			if (er.NumberParameters < 4) {
				return {};
			}
			const auto obj = er.ExceptionInformation[1];
			const auto ti = er.ExceptionInformation[2];
			const auto ib = er.ExceptionInformation[3];
			if (!readable(reinterpret_cast<const void*>(ti), 16)) {
				return {};
			}
			const auto cta = ib + *reinterpret_cast<const std::uint32_t*>(ti + 12);
			if (!readable(reinterpret_cast<const void*>(cta), 8)) {
				return {};
			}
			const auto  count = *reinterpret_cast<const std::int32_t*>(cta);
			std::string types;
			bool        isStd = false;
			for (std::int32_t i = 0; i < count && i < 8; ++i) {
				const auto ct = ib + *reinterpret_cast<const std::uint32_t*>(cta + 4 + 4ull * i);
				if (!readable(reinterpret_cast<const void*>(ct), 8)) {
					break;
				}
				const auto td = ib + *reinterpret_cast<const std::uint32_t*>(ct + 4);
				const char* name = reinterpret_cast<const char*>(td + 16);
				if (!readable(name, 64)) {
					break;
				}
				std::string n(name, strnlen(name, 64));
				isStd = isStd || n == ".?AVexception@std@@";
				types += (types.empty() ? "" : ", ") + n;
			}
			std::string out = "type " + types;
			if (isStd && readable(reinterpret_cast<const void*>(obj + 8), 8)) {
				const char* what = *reinterpret_cast<const char* const*>(obj + 8);
				if (readable(what, 1)) {
					std::size_t len = 0;
					while (len < 200 && readable(what + len, 1) && what[len]) {
						++len;
					}
					out += " what=\"" + std::string(what, len) + "\"";
				}
			}
			return out;
		}

		bool poisoned_register(const CONTEXT& c, std::string& out)
		{
			const DWORD64 regs[] = { c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15 };
			static const char* const names[] = { "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
			const std::uintptr_t eb = exe_base();
			for (std::size_t i = 0; i < std::size(regs); ++i) {
				// Either the full address (base + poisoned offset) or the bare offset may be in a register.
				const std::uint64_t full = regs[i] - eb;
				const std::uint64_t off = (full & 0xFFFF000000000000ull) == kMissingOffset ? full : regs[i];
				if ((off & 0xFFFF000000000000ull) == kMissingOffset) {
					char buf[160];
					std::snprintf(buf, sizeof(buf), "%s holds a poisoned address: missing Address Library ID %llu (+ displacement, if any)",
						names[i], static_cast<unsigned long long>(off & 0x0000FFFFFFFFFFFFull));
					out = buf;
					return true;
				}
			}
			return false;
		}

		LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep)
		{
			if (t_inside || !ep || !ep->ExceptionRecord || !ep->ContextRecord) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			const auto& er = *ep->ExceptionRecord;
			const DWORD code = er.ExceptionCode;
			if (code == 0x406D1388 || code == DBG_PRINTEXCEPTION_C || code == 0x4001000A || code == DBG_CONTROL_C ||
				g_events >= static_cast<LONG>(kMaxEvents)) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			t_inside = true;
			AcquireSRWLockShared(&g_lock);

			const auto     ip = reinterpret_cast<std::uintptr_t>(er.ExceptionAddress);
			std::uintptr_t frames[kMaxFrames]{};
			const unsigned n = unwind(*ep->ContextRecord, frames);
			const Hosted*  culprit = hosted_at(ip);
			const Hosted*  onStack = nullptr;
			for (unsigned i = 0; i < n && !onStack; ++i) {
				onStack = hosted_at(frames[i]);
			}
			std::string poison;
			const bool  isPoison = poisoned_register(*ep->ContextRecord, poison);

			// Deduplicate on (code, first hosted frame or ip).
			std::uintptr_t key = ip;
			for (unsigned i = 0; i < n; ++i) {
				if (hosted_at(frames[i])) {
					key = frames[i];
					break;
				}
			}
			bool seen = false;
			for (LONG i = 0; i < g_events && !seen; ++i) {
				seen = g_seen[i].first == code && g_seen[i].second == key;
			}

			if ((culprit || onStack || isPoison) && !seen) {
				const LONG slot = InterlockedIncrement(&g_events) - 1;
				if (slot < static_cast<LONG>(kMaxEvents)) {
					g_seen[slot] = { code, key };
					logf("[fault] first-chance exception 0x%08lX at %s (thread %lu)%s", static_cast<unsigned long>(code), where(ip).c_str(),
						static_cast<unsigned long>(GetCurrentThreadId()),
						culprit ? "  <- inside a hosted plugin" : (onStack ? "  <- called from a hosted plugin" : ""));
					if (code == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2) {
						const auto kind = er.ExceptionInformation[0];
						logf("[fault]   %s address 0x%llX%s", kind == 0 ? "read" : (kind == 1 ? "write" : "execute"),
							static_cast<unsigned long long>(er.ExceptionInformation[1]),
							er.ExceptionInformation[1] == ~0ull ? " (non-canonical address)" : "");
					} else if (code == 0xE06D7363) {
						logf("[fault]   C++ exception %s", cxx_info(er).c_str());
					}
					if (isPoison) {
						logf("[fault]   %s", poison.c_str());
					}
					for (unsigned i = 0; i < n; ++i) {
						logf("[fault]   #%02u %s", i, where(frames[i]).c_str());
					}
					if (slot + 1 == static_cast<LONG>(kMaxEvents)) {
						logf("[fault] event limit (%zu) reached; further exceptions are not logged", kMaxEvents);
					}
				}
			}

			ReleaseSRWLockShared(&g_lock);
			t_inside = false;
			return EXCEPTION_CONTINUE_SEARCH;
		}
	}

	void fault_log_install(const std::vector<IdEntry>& installed)
	{
		(void)exe_base();  // build the exe section table now, not inside the exception handler
		AcquireSRWLockExclusive(&g_lock);
		g_byOffset.clear();
		g_byOffset.reserve(installed.size());
		for (const auto& e : installed) {
			g_byOffset.emplace_back(e.offset, e.id);
		}
		std::sort(g_byOffset.begin(), g_byOffset.end());
		ReleaseSRWLockExclusive(&g_lock);
		if (!g_handle) {
			g_handle = AddVectoredExceptionHandler(1, &on_exception);
		}
		logf("fault log: %s (log-only, first-chance, max %zu events)", g_handle ? "installed" : "FAILED to install", kMaxEvents);
	}

	void fault_log_add_module(HMODULE mod, const std::string& name)
	{
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const std::uint8_t*>(mod) + dos->e_lfanew);
		Hosted      h{};
		h.base = reinterpret_cast<std::uintptr_t>(mod);
		h.end = h.base + nt->OptionalHeader.SizeOfImage;
		std::snprintf(h.name, sizeof(h.name), "%s", name.c_str());
		AcquireSRWLockExclusive(&g_lock);
		g_hosted.push_back(h);
		ReleaseSRWLockExclusive(&g_lock);
	}
}
