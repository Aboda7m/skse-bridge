// TEST-ONLY trace logger (not for release builds).
//
// Compiled in only when the build is configured with `xmake f --trace=y` (defines SKB_TRACE=1). In a normal
// build every SKB_TRACE_DO(...) call site expands to nothing, so the release DLL contains no tracing code at all.
// Even in a trace build nothing is written unless LegacyHost.ini has [Trace] enabled=1.
//
// Output (next to SKSE-Bridge.log):
//   SKSE-Bridge-trace.jsonl              one JSON object per line: run/plugin lifecycle, table stats, patches, faults,
//                                        and (optional) one "probe" per read the plugin makes in its Address Library table
//   SKSE-Bridge-trace-table-<dll>.csv    per-ID translation result for each hosted plugin (see [Trace] table_dump)
//   plus (see [Trace] flows) "flow" events: shadow SkyrimVM / PlayerCharacter redirects, plugin messaging and co-save traffic.
// Visualise with tools/trace_viz.py or tools/trace_replay.html (table view and node-graph view). See docs/TRACE.md.
#pragma once

#include "common.h"

#ifndef SKB_TRACE
#	define SKB_TRACE 0
#endif

#if SKB_TRACE

namespace bridge::trace
{
	// Tiny JSON fragment builder: Fields().s("k","v").u("n",1) -> "k":"v","n":1
	class Fields
	{
	public:
		Fields& s(const char* key, std::string_view value);
		Fields& u(const char* key, std::uint64_t value);
		Fields& i(const char* key, std::int64_t value);
		Fields& b(const char* key, bool value);
		Fields& x(const char* key, std::uint64_t value);  // hex string "0x..."
		[[nodiscard]] const std::string& str() const { return _text; }

	private:
		void key(const char* k);
		std::string _text;
	};

	// ---- lifecycle ----
	void init(const std::wstring& iniPath);  // reads [Trace]; opens the output; no-op unless enabled=1
	void shutdown();                          // flushes the probe ring and writes run_end
	[[nodiscard]] bool enabled();
	[[nodiscard]] bool want_patches();
	[[nodiscard]] bool want_flows();          // [Trace] flows: shadow VM redirects, messaging, co-save
	[[nodiscard]] int  table_dump_mode();     // 0 none, 1 changed rows only, 2 every row

	// ---- events ----
	// {"t":<ms since start>,"seq":N,"ev":type,"plugin":plugin,<fields>}
	void emit(const char* type, std::string_view plugin, const Fields& f = {});
	// Same, with an explicit timestamp (ms since start, see ms_from_qpc). For events reconstructed after the fact.
	void emit_at(double tMs, const char* type, std::string_view plugin, const Fields& f = {});
	[[nodiscard]] double ms_from_qpc(std::int64_t qpc);

	// For events that can repeat thousands of times (one message delivery, ...). Keeps a running count per `key`;
	// writes the event for the first 8 occurrences, then at most once per 100 ms, adding "n" (running total) and
	// "d" (occurrences since the previous line for this key). A background thread writes any pending remainder, so
	// nothing is lost, only batched. Not for use inside exception handlers (heap + lock).
	void emit_counted(const char* type, std::string_view plugin, std::string_view key, const Fields& f = {});

	// Per-ID translation result of one hosted plugin's private table (CSV). status: translated|missing|remapped.
	// legacy = the plugin's own library, current = the installed (game) library, table = what the plugin will read.
	// All three sorted by ID; table has one row per legacy row, same order.
	void table_dump(std::string_view plugin, const std::vector<IdEntry>& legacy, const std::vector<IdEntry>& current,
		const std::vector<IdEntry>& table);

	// ---- runtime lookup probes ----
	// Called from the MapViewOfFile hook when a hosted plugin maps its private 16-byte-row table. The view is
	// guard-paged; every read of it is recorded as a probe (entry index + ID found there) from an exception handler.
	void on_table_view(const char* plugin, void* base, std::size_t bytes);
}

namespace bridge
{
	// vmshadow.cpp: turns the shadow handler's per-member counters into "vm_access" events. Called by the trace
	// writer thread every ~40 ms (and at shutdown), so the exception handler itself does no extra work.
	void vm_trace_flush();
}

#	define SKB_TRACE_DO(...)                       \
		do {                                        \
			if (::bridge::trace::enabled()) {       \
				__VA_ARGS__;                        \
			}                                       \
		} while (0)

#else

#	define SKB_TRACE_DO(...) \
		do {                  \
		} while (0)

#endif
