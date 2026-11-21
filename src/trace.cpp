// TEST-ONLY trace logger. Whole file is empty unless built with SKB_TRACE=1 (xmake f --trace=y). See trace.h.
#include "trace.h"

#include <map>

#if SKB_TRACE

namespace bridge::trace
{
	namespace
	{
		// ---- settings ----
		bool g_on = false;
		bool g_patches = true;
		bool g_lookups = true;
		bool g_flows = true;
		int  g_table = 1;

		// ---- output ----
		HANDLE         g_file = INVALID_HANDLE_VALUE;
		SRWLOCK        g_lock = SRWLOCK_INIT;
		LARGE_INTEGER  g_freq{};
		LARGE_INTEGER  g_t0{};
		volatile LONG64 g_seq = 0;
		std::wstring   g_dir;
		std::string    g_runId;

		double ms_since_start(std::int64_t qpc)
		{
			return static_cast<double>(qpc - g_t0.QuadPart) * 1000.0 / static_cast<double>(g_freq.QuadPart);
		}

		double now_ms()
		{
			LARGE_INTEGER n{};
			QueryPerformanceCounter(&n);
			return ms_since_start(n.QuadPart);
		}

		void append_json_string(std::string& out, std::string_view s)
		{
			for (const char ch : s) {
				const auto c = static_cast<unsigned char>(ch);
				switch (c) {
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (c < 0x20) {
						char buf[8];
						std::snprintf(buf, sizeof(buf), "\\u%04x", c);
						out += buf;
					} else {
						out += ch;  // UTF-8 bytes pass through
					}
				}
			}
		}

		void write_all(const std::string& s)
		{
			if (s.empty() || g_file == INVALID_HANDLE_VALUE) {
				return;
			}
			AcquireSRWLockExclusive(&g_lock);
			DWORD written = 0;
			WriteFile(g_file, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
			ReleaseSRWLockExclusive(&g_lock);
		}

		// ---- repeated events (emit_counted): first few verbatim, then batched per key ----
		struct Counted
		{
			std::string   type;
			std::string   plugin;
			std::string   fields;   // JSON fragment captured from the first call for this key
			std::uint64_t n{ 0 };
			std::uint64_t reported{ 0 };
			double        lastMs{ -1e9 };
		};
		std::map<std::string, Counted> g_counted;
		SRWLOCK                        g_cntLock = SRWLOCK_INIT;
		constexpr std::uint64_t        kVerbatim = 8;
		constexpr double               kBatchMs = 100.0;

		// ---- plugin names (index goes into the probe ring) ----
		std::vector<std::string> g_pluginsJson;  // JSON-escaped, guarded by g_nameLock
		SRWLOCK                  g_nameLock = SRWLOCK_INIT;

		std::uint32_t plugin_index(std::string_view name)
		{
			std::string esc;
			append_json_string(esc, name);
			AcquireSRWLockExclusive(&g_nameLock);
			for (std::size_t i = 0; i < g_pluginsJson.size(); ++i) {
				if (g_pluginsJson[i] == esc) {
					ReleaseSRWLockExclusive(&g_nameLock);
					return static_cast<std::uint32_t>(i);
				}
			}
			g_pluginsJson.push_back(esc);
			const auto idx = static_cast<std::uint32_t>(g_pluginsJson.size() - 1);
			ReleaseSRWLockExclusive(&g_nameLock);
			return idx;
		}

		std::string plugin_json(std::uint32_t idx)
		{
			std::string r;
			AcquireSRWLockShared(&g_nameLock);
			if (idx < g_pluginsJson.size()) {
				r = g_pluginsJson[idx];
			}
			ReleaseSRWLockShared(&g_nameLock);
			return r;
		}

		// ---- probe ring: filled from the exception handler (no heap, no locks), drained by a writer thread ----
		struct Slot
		{
			volatile LONG64 ready;  // = slot sequence + 1 once the fields below are valid
			std::int64_t    qpc;
			std::uint64_t   id;
			std::uint32_t   tid;
			std::uint32_t   entry;
			std::uint32_t   plugin;
			std::uint32_t   pad;
		};
		constexpr std::size_t kRing = 1u << 17;
		Slot                  g_ring[kRing];
		volatile LONG64       g_head = 0;
		volatile LONG64       g_tail = 0;
		volatile LONG64       g_dropped = 0;
		LONG64                g_total = 0;  // drain thread only

		void ring_push(std::uint32_t plugin, std::uint32_t entry, std::uint64_t id)
		{
			LONG64 h = 0;
			for (;;) {
				h = g_head;
				if (h - g_tail >= static_cast<LONG64>(kRing)) {
					InterlockedIncrement64(&g_dropped);
					return;
				}
				if (InterlockedCompareExchange64(&g_head, h + 1, h) == h) {
					break;
				}
			}
			Slot& s = g_ring[static_cast<std::size_t>(h) % kRing];
			LARGE_INTEGER n{};
			QueryPerformanceCounter(&n);
			s.qpc = n.QuadPart;
			s.id = id;
			s.tid = GetCurrentThreadId();
			s.entry = entry;
			s.plugin = plugin;
			InterlockedExchange64(&s.ready, h + 1);
		}

		void drain_ring()
		{
			std::string batch;
			batch.reserve(1u << 16);
			LONG64 tail = g_tail;
			const LONG64 head = g_head;
			char line[256];
			while (tail < head) {
				Slot& s = g_ring[static_cast<std::size_t>(tail) % kRing];
				if (s.ready != tail + 1) {
					break;  // producer still filling this slot
				}
				const std::string name = plugin_json(s.plugin);
				const LONG64      seq = InterlockedIncrement64(&g_seq);
				std::snprintf(line, sizeof(line), "{\"t\":%.3f,\"seq\":%lld,\"ev\":\"probe\",\"plugin\":\"", ms_since_start(s.qpc),
					static_cast<long long>(seq));
				batch += line;
				batch += name;
				std::snprintf(line, sizeof(line), "\",\"tid\":%u,\"entry\":%u,\"id\":%llu}\n", s.tid, s.entry,
					static_cast<unsigned long long>(s.id));
				batch += line;
				s.ready = 0;
				++tail;
				g_tail = tail;
				++g_total;
				if (batch.size() > 60000) {
					write_all(batch);
					batch.clear();
				}
			}
			write_all(batch);
		}

		void flush_counted(bool force);

		DWORD WINAPI drain_thread(LPVOID)
		{
			double lastBeat = 0;
			for (;;) {
				Sleep(40);
				drain_ring();
				if (g_flows) {
					flush_counted(false);
					bridge::vm_trace_flush();
				}
				const double t = now_ms();
				if (t - lastBeat >= 5000.0) {
					lastBeat = t;
					emit("heartbeat", {}, Fields().u("probes", static_cast<std::uint64_t>(g_total)).u("probes_dropped", static_cast<std::uint64_t>(g_dropped)));
				}
			}
		}

		// ---- guard-paged views of hosted plugins' private tables ----
		struct View
		{
			std::uintptr_t base;
			std::size_t    bytes;
			std::uint32_t  plugin;
			DWORD          protect;
		};
		constexpr int   kMaxViews = 32;
		View            g_views[kMaxViews];
		volatile LONG   g_viewCount = 0;
		void*           g_veh = nullptr;
		constexpr std::uintptr_t kPage = 0x1000;

		struct Rearm
		{
			std::uintptr_t page;
			DWORD          protect;
		};
		thread_local Rearm t_rearm[2];
		thread_local int   t_nrearm = 0;

		constexpr DWORD kGuardPageViolation = 0x80000001u;  // STATUS_GUARD_PAGE_VIOLATION
		constexpr DWORD kTrapFlag = 0x100u;

		LONG CALLBACK on_trace_exception(EXCEPTION_POINTERS* ep)
		{
			if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			const auto& er = *ep->ExceptionRecord;
			if (er.ExceptionCode == kGuardPageViolation) {
				if (er.NumberParameters < 2) {
					return EXCEPTION_CONTINUE_SEARCH;
				}
				const auto addr = static_cast<std::uintptr_t>(er.ExceptionInformation[1]);
				const LONG n = g_viewCount;
				for (LONG i = 0; i < n; ++i) {
					const View& v = g_views[i];
					if (addr < v.base || addr >= v.base + v.bytes) {
						continue;
					}
					// The system cleared PAGE_GUARD on this page when it raised the exception, so it can be read now.
					const std::uint64_t entry = (addr - v.base) / sizeof(IdEntry);
					const std::uint64_t id = reinterpret_cast<const IdEntry*>(v.base)[entry].id;
					ring_push(v.plugin, static_cast<std::uint32_t>(entry), id);
					if (t_nrearm < 2) {
						t_rearm[t_nrearm++] = { addr & ~(kPage - 1), v.protect };
					}
					ep->ContextRecord->EFlags |= kTrapFlag;  // run exactly one instruction, then re-arm the page
					return EXCEPTION_CONTINUE_EXECUTION;
				}
				return EXCEPTION_CONTINUE_SEARCH;
			}
			if (er.ExceptionCode == EXCEPTION_SINGLE_STEP && t_nrearm > 0) {
				for (int i = 0; i < t_nrearm; ++i) {
					DWORD old = 0;
					VirtualProtect(reinterpret_cast<void*>(t_rearm[i].page), kPage, t_rearm[i].protect | PAGE_GUARD, &old);
				}
				t_nrearm = 0;
				ep->ContextRecord->EFlags &= ~kTrapFlag;
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			return EXCEPTION_CONTINUE_SEARCH;
		}

		const char* status_name(const IdEntry& row, const IdEntry* cur)
		{
			if ((row.offset & kMissingOffset) == kMissingOffset) {
				return "missing";
			}
			if (!cur || cur->offset != row.offset) {
				return "remapped";
			}
			return "translated";
		}
	}

	// ---------------- Fields ----------------
	void Fields::key(const char* k)
	{
		if (!_text.empty()) {
			_text += ',';
		}
		_text += '"';
		_text += k;
		_text += "\":";
	}

	Fields& Fields::s(const char* k, std::string_view v)
	{
		key(k);
		_text += '"';
		append_json_string(_text, v);
		_text += '"';
		return *this;
	}

	Fields& Fields::u(const char* k, std::uint64_t v)
	{
		key(k);
		_text += std::to_string(v);
		return *this;
	}

	Fields& Fields::i(const char* k, std::int64_t v)
	{
		key(k);
		_text += std::to_string(v);
		return *this;
	}

	Fields& Fields::b(const char* k, bool v)
	{
		key(k);
		_text += v ? "true" : "false";
		return *this;
	}

	Fields& Fields::x(const char* k, std::uint64_t v)
	{
		char buf[32];
		std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
		return s(k, buf);
	}

	// ---------------- public API ----------------
	bool enabled() { return g_on; }
	bool want_patches() { return g_on && g_patches; }
	bool want_flows() { return g_on && g_flows; }
	double ms_from_qpc(std::int64_t qpc) { return g_on ? ms_since_start(qpc) : 0.0; }
	int  table_dump_mode() { return g_on ? g_table : 0; }

	void emit(const char* type, std::string_view plugin, const Fields& f) { emit_at(g_on ? now_ms() : 0.0, type, plugin, f); }

	void emit_at(double tMs, const char* type, std::string_view plugin, const Fields& f)
	{
		if (!g_on) {
			return;
		}
		const LONG64 seq = InterlockedIncrement64(&g_seq);
		char         head[96];
		std::snprintf(head, sizeof(head), "{\"t\":%.3f,\"seq\":%lld,\"ev\":\"%s\"", tMs, static_cast<long long>(seq), type);
		std::string line = head;
		if (!plugin.empty()) {
			line += ",\"plugin\":\"";
			append_json_string(line, plugin);
			line += '"';
		}
		if (!f.str().empty()) {
			line += ',';
			line += f.str();
		}
		line += "}\n";
		write_all(line);
	}

	namespace
	{
		// One line for a counted key: the captured fields plus n (running total) and d (since the previous line).
		void write_counted(const Counted& c, std::uint64_t n, std::uint64_t d, double tMs)
		{
			char         head[96];
			const LONG64 seq = InterlockedIncrement64(&g_seq);
			std::snprintf(head, sizeof(head), "{\"t\":%.3f,\"seq\":%lld,\"ev\":\"%s\"", tMs, static_cast<long long>(seq), c.type.c_str());
			std::string line = head;
			if (!c.plugin.empty()) {
				line += ",\"plugin\":\"";
				append_json_string(line, c.plugin);
				line += '"';
			}
			if (!c.fields.empty()) {
				line += ',';
				line += c.fields;
			}
			line += ",\"n\":" + std::to_string(n) + ",\"d\":" + std::to_string(d) + "}\n";
			write_all(line);
		}

		void flush_counted(bool force)
		{
			struct Pending
			{
				Counted       c;
				std::uint64_t n, d;
			};
			const double         now = now_ms();
			std::vector<Pending> out;
			AcquireSRWLockExclusive(&g_cntLock);
			for (auto& [key, c] : g_counted) {
				if (c.n > c.reported && (force || now - c.lastMs >= kBatchMs)) {
					out.push_back({ c, c.n, c.n - c.reported });
					c.reported = c.n;
					c.lastMs = now;
				}
			}
			ReleaseSRWLockExclusive(&g_cntLock);
			for (const auto& p : out) {
				write_counted(p.c, p.n, p.d, now);
			}
		}
	}

	void emit_counted(const char* type, std::string_view plugin, std::string_view key, const Fields& f)
	{
		if (!g_on || !g_flows) {
			return;
		}
		const double  now = now_ms();
		Counted       snapshot;
		std::uint64_t n = 0, d = 0;
		bool          write = false;
		AcquireSRWLockExclusive(&g_cntLock);
		auto it = g_counted.find(std::string(key));
		if (it == g_counted.end()) {
			Counted c;
			c.type = type;
			c.plugin = std::string(plugin);
			c.fields = f.str();
			it = g_counted.emplace(std::string(key), std::move(c)).first;
		}
		Counted& c = it->second;
		++c.n;
		if (c.n <= kVerbatim || now - c.lastMs >= kBatchMs) {
			snapshot = c;
			n = c.n;
			d = c.n - c.reported;
			c.reported = c.n;
			c.lastMs = now;
			write = true;
		}
		ReleaseSRWLockExclusive(&g_cntLock);
		if (write) {
			write_counted(snapshot, n, d, now);
		}
	}

	void init(const std::wstring& iniPath)
	{
		if (g_on) {
			return;
		}
		g_on = GetPrivateProfileIntW(L"Trace", L"enabled", 0, iniPath.c_str()) != 0;
		if (!g_on) {
			logf("trace: built in but [Trace] enabled=0");
			return;
		}
		g_lookups = GetPrivateProfileIntW(L"Trace", L"lookups", 1, iniPath.c_str()) != 0;
		g_patches = GetPrivateProfileIntW(L"Trace", L"patches", 1, iniPath.c_str()) != 0;
		g_flows = GetPrivateProfileIntW(L"Trace", L"flows", 1, iniPath.c_str()) != 0;
		g_table = static_cast<int>(GetPrivateProfileIntW(L"Trace", L"table_dump", 1, iniPath.c_str()));
		if (g_table < 0 || g_table > 2) {
			g_table = 1;
		}

		QueryPerformanceFrequency(&g_freq);
		QueryPerformanceCounter(&g_t0);
		g_dir = log_dir();

		SYSTEMTIME st{};
		GetLocalTime(&st);
		char id[48];
		std::snprintf(id, sizeof(id), "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		g_runId = id;

		g_file = CreateFileW((g_dir + L"SKSE-Bridge-trace.jsonl").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (g_file == INVALID_HANDLE_VALUE) {
			logf("trace: cannot create SKSE-Bridge-trace.jsonl (GetLastError=%lu); tracing off", static_cast<unsigned long>(GetLastError()));
			g_on = false;
			return;
		}

		Version gv{};
		const bool haveGame = exe_product_version(gv);
		emit("run_start", {},
			Fields()
				.s("run_id", g_runId)
				.s("build", "trace")
				.s("game_version", haveGame ? gv.str('.') : std::string("unknown"))
				.b("lookups", g_lookups)
				.b("patches", g_patches)
				.b("flows", g_flows)
				.u("table_dump", static_cast<std::uint64_t>(g_table)));

		if (g_lookups) {
			g_veh = AddVectoredExceptionHandler(1, &on_trace_exception);
			if (!g_veh) {
				logf("trace: AddVectoredExceptionHandler failed; lookup probes off");
				g_lookups = false;
			}
		}
		if (g_lookups || g_flows) {
			CreateThread(nullptr, 0, &drain_thread, nullptr, 0, nullptr);  // writes probes, batched flow events and heartbeats
		}
		logf("trace: ON  -> %s (lookups=%d patches=%d flows=%d table_dump=%d). TEST BUILD ONLY: lookups are slow by design.",
			narrow(g_dir + L"SKSE-Bridge-trace.jsonl").c_str(), g_lookups ? 1 : 0, g_patches ? 1 : 0, g_flows ? 1 : 0, g_table);
	}

	void shutdown()
	{
		if (!g_on) {
			return;
		}
		drain_ring();
		if (g_flows) {
			bridge::vm_trace_flush();
			flush_counted(true);
		}
		emit("heartbeat", {}, Fields().u("probes", static_cast<std::uint64_t>(g_total)).u("probes_dropped", static_cast<std::uint64_t>(g_dropped)));
	}

	void table_dump(std::string_view plugin, const std::vector<IdEntry>& legacy, const std::vector<IdEntry>& current,
		const std::vector<IdEntry>& table)
	{
		if (!g_on) {
			return;
		}
		std::uint64_t cTranslated = 0, cRemapped = 0, cMissing = 0;
		std::string   csv = "id,legacy_offset,new_offset,status\n";
		std::size_t   ci = 0;
		const std::size_t n = std::min(legacy.size(), table.size());
		for (std::size_t i = 0; i < n; ++i) {
			const IdEntry& row = table[i];
			while (ci < current.size() && current[ci].id < row.id) {
				++ci;
			}
			const IdEntry* cur = (ci < current.size() && current[ci].id == row.id) ? &current[ci] : nullptr;
			const char*    st = status_name(row, cur);
			const bool     isMissing = st[0] == 'm';
			const bool     isTranslated = st[0] == 't';
			isMissing ? ++cMissing : (isTranslated ? ++cTranslated : ++cRemapped);
			if (g_table == 2 || !isTranslated) {
				char line[128];
				if (isMissing) {
					std::snprintf(line, sizeof(line), "%llu,%llu,,%s\n", static_cast<unsigned long long>(row.id),
						static_cast<unsigned long long>(legacy[i].offset), st);
				} else {
					std::snprintf(line, sizeof(line), "%llu,%llu,%llu,%s\n", static_cast<unsigned long long>(row.id),
						static_cast<unsigned long long>(legacy[i].offset), static_cast<unsigned long long>(row.offset), st);
				}
				csv += line;
			}
		}
		std::string file = "SKSE-Bridge-trace-table-";
		for (const char ch : plugin) {
			file += (ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') ? '_' : ch;
		}
		file += ".csv";
		const HANDLE h = g_table == 0 ? INVALID_HANDLE_VALUE
		                              : CreateFileW((g_dir + widen(file)).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
										FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE) {
			std::size_t done = 0;
			while (done < csv.size()) {
				DWORD w = 0;
				if (!WriteFile(h, csv.data() + done, static_cast<DWORD>(std::min<std::size_t>(csv.size() - done, 1u << 24)), &w, nullptr) || !w) {
					break;
				}
				done += w;
			}
			CloseHandle(h);
		}
		emit("table", plugin,
			Fields()
				.u("rows", n)
				.u("translated", cTranslated)
				.u("remapped", cRemapped)
				.u("missing", cMissing)
				.s("csv", g_table == 0 ? std::string() : file)
				.u("csv_mode", static_cast<std::uint64_t>(g_table)));
	}

	void on_table_view(const char* plugin, void* base, std::size_t /*bytes*/)
	{
		if (!g_on) {
			return;
		}
		MEMORY_BASIC_INFORMATION mbi{};
		if (!base || !VirtualQuery(base, &mbi, sizeof(mbi)) || mbi.BaseAddress != base) {
			emit("table_view", plugin, Fields().b("armed", false).s("why", "VirtualQuery failed"));
			return;
		}
		const std::size_t viewBytes = mbi.RegionSize;
		const std::uint32_t idx = plugin_index(plugin);
		const bool canArm = g_lookups && g_viewCount < kMaxViews;
		if (canArm) {
			const LONG slot = g_viewCount;
			g_views[slot] = { reinterpret_cast<std::uintptr_t>(base), viewBytes, idx, mbi.Protect & 0xFF };
			MemoryBarrier();
			g_viewCount = slot + 1;  // published before any page is guarded
			DWORD old = 0;
			if (!VirtualProtect(base, viewBytes, (mbi.Protect & 0xFF) | PAGE_GUARD, &old)) {
				emit("table_view", plugin,
					Fields().b("armed", false).u("bytes", viewBytes).s("why", "VirtualProtect(PAGE_GUARD) failed, GetLastError=" + std::to_string(GetLastError())));
				return;
			}
		}
		emit("table_view", plugin, Fields().b("armed", canArm).u("bytes", viewBytes).u("rows", viewBytes / sizeof(IdEntry)));
	}
}

#endif  // SKB_TRACE
