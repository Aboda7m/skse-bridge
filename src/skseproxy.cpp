// Per-host SKSE interface proxies for hosted legacy plugins.
//
// Problem (observed in game, 2026-11-12): hosted plugins run under the bridge's single SKSE plugin handle.
// SKSE's PluginManager::registerListener skips a listener whose handle is already registered for that
// sender ("is listener already registered?"), so only the FIRST hosted plugin that registers for "SKSE"
// receives messages. skse64.log showed two "registering plugin listener for SKSE" lines but one
// "sending message type N to plugin 1" per message; the second plugin never saw kDataLoaded.
//
// Fix: hosted plugins get a proxy SKSEMessagingInterface. The bridge registers ONE listener per sender
// with SKSE and fans every message out to all hosted listeners for that sender, in registration order.
// Dispatch and GetEventDispatcher are forwarded unchanged.
//
// Hosted plugins talking to each other (observed 2026-11-12: CompassNavigationOverhaul listens to sender
// "InfinityUI"; both hosted): SKSE only knows the bridge, so it refuses a listener for "InfinityUI" and
// InfinityUI's Dispatch reaches nobody ("Failed to dispatch message to all listeners"). The proxy routes
// these inside the bridge: a listener for a hosted plugin's name is kept locally; a hosted plugin's
// Dispatch is delivered to those listeners with the hosted plugin's own name as sender, honouring the
// receiver argument; GetPluginInfo answers for hosted plugins. Non-hosted plugins still cannot reach a
// hosted one by name (SKSE does not know it).
//
// Serialization (co-save) has the same single-handle problem: SKSE keeps one unique ID and one set of
// callbacks per handle. With cosave_multiplex=1 the bridge owns the co-save slot itself (unique ID 'SKBR')
// and gives every hosted plugin its own sub-space inside it: each hosted record is stored as an SKSE record
// whose type is the hosted plugin's unique ID, whose version is the hosted record's version, and whose data
// is [hosted record type (4 bytes)][hosted data]. Load splits them back and calls each hosted plugin's load
// callback with only its own records. Records of hosted plugins that are not installed are kept and written
// back unchanged, so removing a plugin for one session does not erase its data. With cosave_multiplex=0 the
// proxy forwards unchanged and warns when more than one hosted plugin claims the co-save.
//
// Layouts: CommonLibSSE-NG 539d4ce include/SKSE/Impl/Stubs.h (SKSEMessagingInterface,
// SKSESerializationInterface), include/SKSE/Interfaces.h:261-267 (Message), interface IDs
// Interfaces.h:42-52 (kSerialization = 3, kMessaging = 5). [Verified]
#include "common.h"
#include "trace.h"

#include <deque>

#if defined(_MSC_VER)
#	include <intrin.h>
#	define BRIDGE_CALLER() _ReturnAddress()
#else
#	define BRIDGE_CALLER() __builtin_return_address(0)
#endif

namespace bridge
{
	namespace
	{
		constexpr std::uint32_t kSerialization = 3;
		constexpr std::uint32_t kMessaging = 5;

		struct Message
		{
			const char*   sender;
			std::uint32_t type;
			std::uint32_t dataLen;
			void*         data;
		};
		using Callback = void (*)(Message*);

		struct MessagingIface
		{
			std::uint32_t interfaceVersion;
			bool (*RegisterListener)(std::uint32_t, const char*, void*);
			bool (*Dispatch)(std::uint32_t, std::uint32_t, void*, std::uint32_t, const char*);
			void* (*GetEventDispatcher)(std::uint32_t);
		};

		struct SerializationIface
		{
			std::uint32_t version;
			void (*SetUniqueID)(std::uint32_t, std::uint32_t);
			void (*SetRevertCallback)(std::uint32_t, void*);
			void (*SetSaveCallback)(std::uint32_t, void*);
			void (*SetLoadCallback)(std::uint32_t, void*);
			void (*SetFormDeleteCallback)(std::uint32_t, void*);
			void* rest[7];  // WriteRecord .. ResolveFormId: forwarded as-is
		};

		struct Listener
		{
			std::string sender;  // empty = every sender
			Callback    cb;
			std::string plugin;
			std::uint32_t seenTypes[16]{};  // message types already logged for this listener
			unsigned    seenCount{ 0 };
		};

		struct PluginInfoRec
		{
			std::uint32_t infoVersion;
			const char*   name;
			std::uint32_t version;
		};

		struct Declared
		{
			std::string   file;    // DLL file name
			std::string   plugin;  // SKSEPlugin_Version name
			std::string   key;     // lowercase plugin name
			PluginInfoRec info{};
		};

		struct Module
		{
			std::uintptr_t  base, end;
			std::string     name;
			const Declared* decl;
		};

		const SKSEInterface*      g_real = nullptr;
		void* (*g_realQuery)(std::uint32_t) = nullptr;
		std::uint32_t             g_hostHandle = 0;
		bool                      g_fanout = true;
		MessagingIface*           g_realMsg = nullptr;
		MessagingIface            g_proxyMsg{};
		SerializationIface*       g_realSer = nullptr;
		SerializationIface        g_proxySer{};
		bool                      g_mux = false;  // co-save multiplexing
		SRWLOCK                   g_lock = SRWLOCK_INIT;
		std::vector<Listener>     g_listeners;
		std::vector<std::string>  g_registered;  // senders the bridge registered with SKSE (lowercase; "" = all)
		std::vector<Module>       g_modules;
		std::vector<std::string>  g_serializationOwners;
		std::deque<Declared>      g_declared;  // deque: stable addresses for Message::sender / PluginInfo::name
		const void* (*g_realInfo)(const char*) = nullptr;
		std::string               g_loading;  // file name of the plugin whose SKSEPlugin_Load is running
		DWORD                     g_loadingThread = 0;

		std::string lower(const char* s)
		{
			std::string r = s ? s : "";
			for (auto& c : r) {
				c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
			}
			return r;
		}

		const Declared* declared_by_name(const std::string& key)
		{
			for (const auto& d : g_declared) {
				if (d.key == key) {
					return &d;
				}
			}
			return nullptr;
		}

		const Module* module_at_addr(std::uintptr_t a)
		{
			for (const auto& m : g_modules) {
				if (a >= m.base && a < m.end) {
					return &m;
				}
			}
			return nullptr;
		}

		// The hosted module that called into the proxy. The return address is tried first; if the plugin
		// reached us through a tail call it points elsewhere, so the nearest hosted frame on the stack is used.
		const Module* module_at(const void* ret)
		{
			if (const Module* m = module_at_addr(reinterpret_cast<std::uintptr_t>(ret))) {
				return m;
			}
			void*        frames[12]{};
			const USHORT n = RtlCaptureStackBackTrace(1, static_cast<DWORD>(std::size(frames)), frames, nullptr);
			for (USHORT i = 0; i < n; ++i) {
				if (const Module* m = module_at_addr(reinterpret_cast<std::uintptr_t>(frames[i]))) {
					return m;
				}
			}
			// Tail call straight out of SKSEPlugin_Load leaves no plugin frame: attribute to the plugin being loaded.
			if (!g_loading.empty() && GetCurrentThreadId() == g_loadingThread) {
				for (const auto& m : g_modules) {
					if (m.name == g_loading) {
						return &m;
					}
				}
			}
			return nullptr;
		}

		std::string caller_name(const void* ret)
		{
			AcquireSRWLockShared(&g_lock);
			const Module*     m = module_at(ret);
			const std::string n = m ? m->name : "<unknown caller>";
			ReleaseSRWLockShared(&g_lock);
			return n;
		}

		void fanout(Message* msg)
		{
			if (!msg) {
				return;
			}
			const std::string from = lower(msg->sender);
			// Copy first: a callback may register further listeners while we iterate.
			std::vector<std::pair<Callback, std::size_t>> targets;
			AcquireSRWLockShared(&g_lock);
			for (std::size_t i = 0; i < g_listeners.size(); ++i) {
				if (g_listeners[i].sender.empty() || g_listeners[i].sender == from) {
					targets.emplace_back(g_listeners[i].cb, i);
				}
			}
			ReleaseSRWLockShared(&g_lock);

			for (const auto& [cb, idx] : targets) {
				AcquireSRWLockExclusive(&g_lock);
				auto& l = g_listeners[idx];
				bool  first = true;
				for (unsigned k = 0; k < l.seenCount; ++k) {
					first = first && l.seenTypes[k] != msg->type;
				}
				if (first && l.seenCount < std::size(l.seenTypes)) {
					l.seenTypes[l.seenCount++] = msg->type;
				}
				const std::string plugin = l.plugin;
				ReleaseSRWLockExclusive(&g_lock);
				if (first) {
					logf("[messaging] delivering message type %u from %s to %s", msg->type, msg->sender ? msg->sender : "?", plugin.c_str());
				}
				SKB_TRACE_DO(if (trace::want_flows()) {
					const std::string snd = msg->sender ? msg->sender : "?";
					trace::emit_counted("msg", plugin, "fanout|" + snd + "|" + plugin + "|" + std::to_string(msg->type),
						trace::Fields().s("from", snd).u("type", msg->type).u("len", msg->dataLen).s("route", "skse"));
				});
				cb(msg);  // no exception filter: a crash here must stay a crash (the fault log records it)
			}
			if (from == "skse" && msg->type == 8) {  // kDataLoaded: most plugins have installed their hooks by now
				patch_audit_flush("after kDataLoaded");
				vm_shadow_report("after kDataLoaded");
				vm_session_reached_game();  // got through startup with the shadow: no reason to re-ask next launch
			}
			if (from == "skse" && (msg->type == 3 || msg->type == 7)) {  // kPostLoadGame / kNewGame
				vm_shadow_report(msg->type == 3 ? "after kPostLoadGame" : "after kNewGame");
				vm_session_reached_game();
			}
		}

		bool proxy_register(std::uint32_t, const char* sender, void* handler)
		{
			const std::string plugin = caller_name(BRIDGE_CALLER());
			if (!handler) {
				return false;
			}
			const std::string key = lower(sender);
			bool              needReal = false;
			AcquireSRWLockExclusive(&g_lock);
			const bool internal = !key.empty() && declared_by_name(key) != nullptr;
			Listener l;
			l.sender = key;
			l.cb = reinterpret_cast<Callback>(handler);
			l.plugin = plugin;
			g_listeners.push_back(std::move(l));
			needReal = !internal && std::find(g_registered.begin(), g_registered.end(), key) == g_registered.end();
			if (needReal) {
				g_registered.push_back(key);
			}
			ReleaseSRWLockExclusive(&g_lock);

			bool ok = true;
			if (needReal) {
				ok = g_realMsg->RegisterListener(g_hostHandle, sender, reinterpret_cast<void*>(&fanout));
				if (!ok) {
					AcquireSRWLockExclusive(&g_lock);
					g_registered.erase(std::find(g_registered.begin(), g_registered.end(), key));
					ReleaseSRWLockExclusive(&g_lock);
				}
			}
			logf("[messaging] %s listens to %s%s", plugin.c_str(), sender ? ("\"" + std::string(sender) + "\"").c_str() : "all senders",
				internal ? " (hosted sender: routed inside the bridge)" :
				(needReal ? (ok ? " (bridge registered with SKSE)" : " (SKSE REFUSED the registration)") : " (shared bridge registration)"));
			SKB_TRACE_DO(trace::emit("msg_listen", plugin, trace::Fields().s("sender", sender ? sender : "").b("internal", internal).b("ok", ok)));
			return ok;
		}

		bool proxy_dispatch(std::uint32_t, std::uint32_t type, void* data, std::uint32_t len, const char* receiver)
		{
			AcquireSRWLockShared(&g_lock);
			const Module*   m = module_at(BRIDGE_CALLER());
			const Declared* from = m ? m->decl : nullptr;
			const Declared* to = receiver ? declared_by_name(lower(receiver)) : nullptr;
			std::vector<std::pair<Callback, std::string>> targets;
			if (from) {
				for (const auto& l : g_listeners) {
					if (l.sender != from->key) {
						continue;
					}
					if (receiver) {
						// receiver is a plugin name; listeners are recorded by DLL file name
						const Declared* ld = nullptr;
						for (const auto& d : g_declared) {
							if (d.file == l.plugin) {
								ld = &d;
							}
						}
						if (!ld || ld != to) {
							continue;
						}
					}
					targets.emplace_back(l.cb, l.plugin);
				}
			}
			ReleaseSRWLockShared(&g_lock);

			if (from) {
				Message msg{ from->plugin.c_str(), type, len, data };
				static volatile LONG logged = 0;
				for (const auto& [cb, plugin] : targets) {
					if (InterlockedIncrement(&logged) <= 32) {
						logf("[messaging] routing message type %u from %s to %s (inside the bridge)", type, from->plugin.c_str(), plugin.c_str());
					}
					SKB_TRACE_DO(if (trace::want_flows()) {
						trace::emit_counted("msg", plugin, "route|" + from->plugin + "|" + plugin + "|" + std::to_string(type),
							trace::Fields().s("from", from->plugin).u("type", type).u("len", len).s("route", "internal"));
					});
					cb(&msg);
				}
			}
			if (to) {
				return !targets.empty();  // addressed to a hosted plugin: SKSE cannot deliver it
			}
			const bool sent = g_realMsg->Dispatch(g_hostHandle, type, data, len, receiver);
			SKB_TRACE_DO(if (from && trace::want_flows()) {
				const std::string rcv = receiver ? receiver : "";
				trace::emit_counted("msg_out", from->file, "out|" + from->plugin + "|" + rcv + "|" + std::to_string(type),
					trace::Fields().s("from", from->plugin).s("to", rcv).u("type", type).u("len", len).b("delivered", sent));
			});
			return sent || !targets.empty();
		}

		const void* proxy_info(const char* name)
		{
			if (name) {
				AcquireSRWLockShared(&g_lock);
				const Declared* d = declared_by_name(lower(name));
				ReleaseSRWLockShared(&g_lock);
				if (d) {
					return &d->info;
				}
			}
			return g_realInfo ? g_realInfo(name) : nullptr;
		}

		void* proxy_dispatcher(std::uint32_t id) { return g_realMsg->GetEventDispatcher(id); }

		void note_serialization(const void* ret, const char* what)
		{
			const std::string plugin = caller_name(ret);
			AcquireSRWLockExclusive(&g_lock);
			const bool known = std::find(g_serializationOwners.begin(), g_serializationOwners.end(), plugin) != g_serializationOwners.end();
			if (!known) {
				g_serializationOwners.push_back(plugin);
			}
			const std::size_t owners = g_serializationOwners.size();
			std::string       list;
			for (const auto& o : g_serializationOwners) {
				list += o + " ";
			}
			ReleaseSRWLockExclusive(&g_lock);
			logf("[serialization] %s: %s", plugin.c_str(), what);
			if (!known && owners > 1 && !g_mux) {
				logf("[serialization] WARNING: %zu hosted plugins use the co-save (%s). SKSE keeps one unique ID and one set of "
					 "callbacks per plugin handle and they all share the bridge's handle: the last one to register wins, the others "
					 "do not save or load their co-save data. Host only one co-save plugin per bridge.",
					owners, list.c_str());
			}
		}

		// ---- co-save multiplexer ----
		constexpr std::uint32_t kMuxUid = (std::uint32_t('S') << 24) | (std::uint32_t('K') << 16) | (std::uint32_t('B') << 8) | std::uint32_t('R');
		using SerCb = void (*)(SerializationIface*);
		using DelCb = void (*)(std::uint64_t);

		struct Slot
		{
			std::string   plugin;
			std::uint32_t uid{ 0 };
			SerCb         revert{ nullptr }, save{ nullptr }, load{ nullptr };
			DelCb         del{ nullptr };
			unsigned      saved{ 0 }, loaded{ 0 };
		};

		struct Rec
		{
			std::uint32_t              uid, version;
			std::vector<std::uint8_t> data;  // [inner type][inner data]
		};

		bool                      g_muxRegistered = false;
		std::vector<Slot>         g_slots;
		std::vector<Rec>          g_orphans;   // records of hosted plugins not present this session
		int                       g_cur = -1;  // slot whose callback is running
		bool                      g_open = false;
		std::uint32_t             g_openType = 0, g_openVer = 0;
		std::vector<std::uint8_t> g_openBuf;
		std::vector<const Rec*>   g_loadList;
		std::size_t               g_loadIdx = 0, g_readPos = 0;
		bool                      g_loadHas = false;

		using WriteRecordFn = bool (*)(std::uint32_t, std::uint32_t, const void*, std::uint32_t);
		using GetNextFn = bool (*)(std::uint32_t*, std::uint32_t*, std::uint32_t*);
		using ReadFn = std::uint32_t (*)(void*, std::uint32_t);
		WriteRecordFn real_write() { return reinterpret_cast<WriteRecordFn>(g_realSer->rest[0]); }
		GetNextFn     real_next() { return reinterpret_cast<GetNextFn>(g_realSer->rest[3]); }
		ReadFn        real_read() { return reinterpret_cast<ReadFn>(g_realSer->rest[4]); }

		std::string fourcc(std::uint32_t v)
		{
			char b[16];
			const auto ch = [](std::uint32_t x) { x &= 0xFF; return static_cast<char>(x >= 0x20 && x < 0x7F ? x : '?'); };
			std::snprintf(b, sizeof(b), "'%c%c%c%c'", ch(v >> 24), ch(v >> 16), ch(v >> 8), ch(v));
			return b;
		}

		bool write_wrapped(std::uint32_t type, std::uint32_t ver, const void* data, std::uint32_t len)
		{
			if (g_cur < 0) {
				return false;
			}
			std::vector<std::uint8_t> buf(4 + static_cast<std::size_t>(len));
			std::memcpy(buf.data(), &type, 4);
			if (len) {
				std::memcpy(buf.data() + 4, data, len);
			}
			const bool ok = real_write()(g_slots[g_cur].uid, ver, buf.data(), static_cast<std::uint32_t>(buf.size()));
			g_slots[g_cur].saved += ok;
			return ok;
		}

		void flush_open()
		{
			if (g_open) {
				g_open = false;
				write_wrapped(g_openType, g_openVer, g_openBuf.data(), static_cast<std::uint32_t>(g_openBuf.size()));
				g_openBuf.clear();
			}
		}

		bool mux_write_record(std::uint32_t type, std::uint32_t ver, const void* data, std::uint32_t len)
		{
			flush_open();
			return write_wrapped(type, ver, data, len);
		}

		bool mux_open_record(std::uint32_t type, std::uint32_t ver)
		{
			if (g_cur < 0) {
				return false;
			}
			flush_open();
			g_open = true;
			g_openType = type;
			g_openVer = ver;
			g_openBuf.clear();
			return true;
		}

		bool mux_write_data(const void* data, std::uint32_t len)
		{
			if (!g_open) {
				return false;
			}
			const auto* p = static_cast<const std::uint8_t*>(data);
			g_openBuf.insert(g_openBuf.end(), p, p + len);
			return true;
		}

		bool mux_next(std::uint32_t* type, std::uint32_t* ver, std::uint32_t* len)
		{
			if (!g_loadHas) {
				return false;
			}
			if (g_loadIdx >= g_loadList.size()) {
				return false;
			}
			const Rec* r = g_loadList[g_loadIdx++];
			std::uint32_t t = 0;
			std::memcpy(&t, r->data.data(), 4);
			if (type) *type = t;
			if (ver) *ver = r->version;
			if (len) *len = static_cast<std::uint32_t>(r->data.size() - 4);
			g_readPos = 4;
			return true;
		}

		std::uint32_t mux_read(void* buf, std::uint32_t len)
		{
			if (!g_loadHas || g_loadIdx == 0) {
				return 0;
			}
			const Rec*        r = g_loadList[g_loadIdx - 1];
			const std::size_t n = std::min<std::size_t>(len, r->data.size() - g_readPos);
			std::memcpy(buf, r->data.data() + g_readPos, n);
			g_readPos += n;
			return static_cast<std::uint32_t>(n);
		}

		void mux_save_cb(SerializationIface*)
		{
			for (std::size_t i = 0; i < g_slots.size(); ++i) {
				if (!g_slots[i].save || !g_slots[i].uid) {
					continue;
				}
				g_cur = static_cast<int>(i);
				g_slots[i].saved = 0;
				g_slots[i].save(&g_proxySer);
				flush_open();
				g_cur = -1;
				logf("[cosave] save: %s %s wrote %u record(s)", g_slots[i].plugin.c_str(), fourcc(g_slots[i].uid).c_str(), g_slots[i].saved);
				SKB_TRACE_DO(trace::emit("cosave", g_slots[i].plugin, trace::Fields().s("phase", "save").s("uid", fourcc(g_slots[i].uid)).u("records", g_slots[i].saved)));
			}
			for (const auto& r : g_orphans) {
				real_write()(r.uid, r.version, r.data.data(), static_cast<std::uint32_t>(r.data.size()));
			}
			if (!g_orphans.empty()) {
				logf("[cosave] save: kept %zu record(s) of hosted plugins not loaded this session", g_orphans.size());
				SKB_TRACE_DO(trace::emit("cosave", {}, trace::Fields().s("phase", "save_orphans").u("records", g_orphans.size())));
			}
		}

		void mux_load_cb(SerializationIface*)
		{
			std::vector<Rec> recs;
			std::uint32_t    uid = 0, ver = 0, len = 0;
			while (real_next()(&uid, &ver, &len)) {
				Rec r{ uid, ver, std::vector<std::uint8_t>(len) };
				const std::uint32_t got = len ? real_read()(r.data.data(), len) : 0;
				r.data.resize(got);
				if (r.data.size() >= 4) {
					recs.push_back(std::move(r));
				}
			}
			g_orphans.clear();
			for (const auto& r : recs) {
				bool owned = false;
				for (const auto& sl : g_slots) {
					owned = owned || (sl.uid == r.uid && sl.load);
				}
				if (!owned) {
					g_orphans.push_back(r);
				}
			}
			for (std::size_t i = 0; i < g_slots.size(); ++i) {
				auto& sl = g_slots[i];
				if (!sl.load || !sl.uid) {
					continue;
				}
				g_loadList.clear();
				for (const auto& r : recs) {
					if (r.uid == sl.uid) {
						g_loadList.push_back(&r);
					}
				}
				if (g_loadList.empty()) {
					continue;  // SKSE calls a load callback only when the plugin has data in the co-save
				}
				g_loadIdx = 0;
				g_readPos = 0;
				g_loadHas = true;
				g_cur = static_cast<int>(i);
				sl.load(&g_proxySer);
				g_cur = -1;
				g_loadHas = false;
				logf("[cosave] load: %s %s got %zu record(s), read %zu", sl.plugin.c_str(), fourcc(sl.uid).c_str(), g_loadList.size(), g_loadIdx);
				SKB_TRACE_DO(trace::emit("cosave", sl.plugin, trace::Fields().s("phase", "load").s("uid", fourcc(sl.uid)).u("records", g_loadList.size()).u("read", g_loadIdx)));
			}
			g_loadList.clear();
			if (!g_orphans.empty()) {
				logf("[cosave] load: %zu record(s) belong to hosted plugins not loaded this session; kept for the next save", g_orphans.size());
				SKB_TRACE_DO(trace::emit("cosave", {}, trace::Fields().s("phase", "load_orphans").u("records", g_orphans.size())));
			}
		}

		void mux_revert_cb(SerializationIface*)
		{
			SKB_TRACE_DO(trace::emit("cosave", {}, trace::Fields().s("phase", "revert")));
			g_orphans.clear();
			for (std::size_t i = 0; i < g_slots.size(); ++i) {
				if (g_slots[i].revert) {
					g_cur = static_cast<int>(i);
					g_slots[i].revert(&g_proxySer);
					g_cur = -1;
				}
			}
		}

		void mux_delete_cb(std::uint64_t handle)
		{
			for (const auto& sl : g_slots) {
				if (sl.del) {
					sl.del(handle);
				}
			}
		}

		void mux_register()
		{
			if (g_muxRegistered) {
				return;
			}
			g_muxRegistered = true;
			g_realSer->SetUniqueID(g_hostHandle, kMuxUid);
			g_realSer->SetRevertCallback(g_hostHandle, reinterpret_cast<void*>(&mux_revert_cb));
			g_realSer->SetSaveCallback(g_hostHandle, reinterpret_cast<void*>(&mux_save_cb));
			g_realSer->SetLoadCallback(g_hostHandle, reinterpret_cast<void*>(&mux_load_cb));
			g_realSer->SetFormDeleteCallback(g_hostHandle, reinterpret_cast<void*>(&mux_delete_cb));
			logf("[cosave] bridge registered co-save %s; hosted plugins get one sub-space each, keyed by their own unique ID",
				fourcc(kMuxUid).c_str());
			SKB_TRACE_DO(trace::emit("cosave", {}, trace::Fields().s("phase", "register").s("uid", fourcc(kMuxUid))));
		}

		Slot& slot_for(const std::string& plugin)
		{
			for (auto& sl : g_slots) {
				if (sl.plugin == plugin) {
					return sl;
				}
			}
			g_slots.push_back({});
			g_slots.back().plugin = plugin;
			return g_slots.back();
		}

		void ser_uid(std::uint32_t h, std::uint32_t uid)
		{
			char buf[64];
			std::snprintf(buf, sizeof(buf), "SetUniqueID('%c%c%c%c')", static_cast<char>(uid >> 24), static_cast<char>(uid >> 16),
				static_cast<char>(uid >> 8), static_cast<char>(uid));
			const void* ret = BRIDGE_CALLER();
			note_serialization(ret, buf);
			if (!g_mux) {
				g_realSer->SetUniqueID(h, uid);
				return;
			}
			const std::string plugin = caller_name(ret);
			for (const auto& sl : g_slots) {
				if (sl.uid == uid && sl.plugin != plugin) {
					logf("[cosave] WARNING: %s and %s both use unique ID %s; their records will be mixed", sl.plugin.c_str(), plugin.c_str(),
						fourcc(uid).c_str());
				}
			}
			if (uid == kMuxUid) {
				logf("[cosave] WARNING: %s uses the bridge's own unique ID %s", plugin.c_str(), fourcc(uid).c_str());
			}
			slot_for(plugin).uid = uid;
			SKB_TRACE_DO(trace::emit("cosave", plugin, trace::Fields().s("phase", "claim").s("uid", fourcc(uid))));
			mux_register();
		}
		template <class F>
		void ser_set(const void* ret, const char* what, std::uint32_t h, void* cb, void (*SerializationIface::*realFn)(std::uint32_t, void*), F apply)
		{
			note_serialization(ret, what);
			if (!g_mux) {
				(g_realSer->*realFn)(h, cb);
				return;
			}
			apply(slot_for(caller_name(ret)), cb);
			mux_register();
		}
		void ser_revert(std::uint32_t h, void* cb)
		{
			ser_set(BRIDGE_CALLER(), "SetRevertCallback", h, cb, &SerializationIface::SetRevertCallback, [](Slot& s, void* c) { s.revert = reinterpret_cast<SerCb>(c); });
		}
		void ser_save(std::uint32_t h, void* cb)
		{
			ser_set(BRIDGE_CALLER(), "SetSaveCallback", h, cb, &SerializationIface::SetSaveCallback, [](Slot& s, void* c) { s.save = reinterpret_cast<SerCb>(c); });
		}
		void ser_load(std::uint32_t h, void* cb)
		{
			ser_set(BRIDGE_CALLER(), "SetLoadCallback", h, cb, &SerializationIface::SetLoadCallback, [](Slot& s, void* c) { s.load = reinterpret_cast<SerCb>(c); });
		}
		void ser_delete(std::uint32_t h, void* cb)
		{
			ser_set(BRIDGE_CALLER(), "SetFormDeleteCallback", h, cb, &SerializationIface::SetFormDeleteCallback, [](Slot& s, void* c) { s.del = reinterpret_cast<DelCb>(c); });
		}

		void* proxy_query(std::uint32_t id)
		{
			if (id == kMessaging && g_fanout && g_realMsg) {
				return &g_proxyMsg;
			}
			if (id == kSerialization && g_realSer) {
				return &g_proxySer;
			}
			return g_realQuery(id);
		}
	}

	void skse_proxy_init(const SKSEInterface* real, bool fanout, bool cosaveMux)
	{
		g_real = real;
		g_realQuery = real->QueryInterface;
		g_realInfo = real->GetPluginInfo;
		g_hostHandle = real->GetPluginHandle();
		g_fanout = fanout;
		g_realMsg = static_cast<MessagingIface*>(g_realQuery(kMessaging));
		if (g_realMsg) {
			g_proxyMsg.interfaceVersion = g_realMsg->interfaceVersion;
			g_proxyMsg.RegisterListener = &proxy_register;
			g_proxyMsg.Dispatch = &proxy_dispatch;
			g_proxyMsg.GetEventDispatcher = &proxy_dispatcher;
		}
		g_realSer = static_cast<SerializationIface*>(g_realQuery(kSerialization));
		if (g_realSer) {
			g_proxySer = *g_realSer;  // copies version and the forwarded record functions
			g_proxySer.SetUniqueID = &ser_uid;
			g_proxySer.SetRevertCallback = &ser_revert;
			g_proxySer.SetSaveCallback = &ser_save;
			g_proxySer.SetLoadCallback = &ser_load;
			g_proxySer.SetFormDeleteCallback = &ser_delete;
			g_mux = cosaveMux;
			if (g_mux) {
				g_proxySer.rest[0] = reinterpret_cast<void*>(&mux_write_record);
				g_proxySer.rest[1] = reinterpret_cast<void*>(&mux_open_record);
				g_proxySer.rest[2] = reinterpret_cast<void*>(&mux_write_data);
				g_proxySer.rest[3] = reinterpret_cast<void*>(&mux_next);
				g_proxySer.rest[4] = reinterpret_cast<void*>(&mux_read);
				// rest[5] ResolveHandle, rest[6] ResolveFormId: forwarded (valid while the bridge's load callback runs)
			}
		}
		logf("skse proxy: host handle %u, messaging %s (v%u), serialization %s", g_hostHandle,
			g_realMsg ? (fanout ? "fan-out" : "pass-through") : "unavailable", g_realMsg ? g_realMsg->interfaceVersion : 0,
			g_realSer ? (g_mux ? "multiplexed" : "monitored") : "unavailable");
	}

	void skse_proxy_add_module(HMODULE mod, const std::string& name)
	{
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const std::uint8_t*>(mod) + dos->e_lfanew);
		AcquireSRWLockExclusive(&g_lock);
		const Declared* decl = nullptr;
		for (const auto& d : g_declared) {
			if (d.file == name) {
				decl = &d;
			}
		}
		g_modules.push_back({ reinterpret_cast<std::uintptr_t>(mod), reinterpret_cast<std::uintptr_t>(mod) + nt->OptionalHeader.SizeOfImage, name, decl });
		ReleaseSRWLockExclusive(&g_lock);
	}

	void* (*skse_proxy_query())(std::uint32_t)
	{
		return g_realQuery ? &proxy_query : nullptr;
	}
}

namespace bridge
{
	void skse_proxy_declare(const std::string& file, const std::string& pluginName, std::uint32_t version)
	{
		if (pluginName.empty()) {
			return;
		}
		AcquireSRWLockExclusive(&g_lock);
		auto& d = g_declared.emplace_back();
		d.file = file;
		d.plugin = pluginName;
		d.key = lower(pluginName.c_str());
		d.info = { 1, d.plugin.c_str(), version };
		ReleaseSRWLockExclusive(&g_lock);
		logf("skse proxy: hosted plugin \"%s\" (%s) declared", pluginName.c_str(), file.c_str());
	}

	const void* (*skse_proxy_plugin_info())(const char*)
	{
		return g_realInfo ? &proxy_info : nullptr;
	}
}

namespace bridge
{
	void skse_proxy_loading(const std::string& file)
	{
		AcquireSRWLockExclusive(&g_lock);
		g_loading = file;
		g_loadingThread = file.empty() ? 0 : GetCurrentThreadId();
		ReleaseSRWLockExclusive(&g_lock);
	}
}
