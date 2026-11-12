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
// callbacks per handle. Multiplexing co-save records is not done here; the proxy forwards unchanged and
// logs a WARNING when more than one hosted plugin claims it.
//
// Layouts: CommonLibSSE-NG 539d4ce include/SKSE/Impl/Stubs.h (SKSEMessagingInterface,
// SKSESerializationInterface), include/SKSE/Interfaces.h:261-267 (Message), interface IDs
// Interfaces.h:42-52 (kSerialization = 3, kMessaging = 5). [Verified]
#include "common.h"

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
				cb(msg);  // no exception filter: a crash here must stay a crash (the fault log records it)
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
					cb(&msg);
				}
			}
			if (to) {
				return !targets.empty();  // addressed to a hosted plugin: SKSE cannot deliver it
			}
			const bool sent = g_realMsg->Dispatch(g_hostHandle, type, data, len, receiver);
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
			if (!known && owners > 1) {
				logf("[serialization] WARNING: %zu hosted plugins use the co-save (%s). SKSE keeps one unique ID and one set of "
					 "callbacks per plugin handle and they all share the bridge's handle: the last one to register wins, the others "
					 "do not save or load their co-save data. Host only one co-save plugin per bridge.",
					owners, list.c_str());
			}
		}

		void ser_uid(std::uint32_t h, std::uint32_t uid)
		{
			char buf[64];
			std::snprintf(buf, sizeof(buf), "SetUniqueID('%c%c%c%c')", static_cast<char>(uid >> 24), static_cast<char>(uid >> 16),
				static_cast<char>(uid >> 8), static_cast<char>(uid));
			note_serialization(BRIDGE_CALLER(), buf);
			g_realSer->SetUniqueID(h, uid);
		}
		void ser_revert(std::uint32_t h, void* cb) { note_serialization(BRIDGE_CALLER(), "SetRevertCallback"); g_realSer->SetRevertCallback(h, cb); }
		void ser_save(std::uint32_t h, void* cb) { note_serialization(BRIDGE_CALLER(), "SetSaveCallback"); g_realSer->SetSaveCallback(h, cb); }
		void ser_load(std::uint32_t h, void* cb) { note_serialization(BRIDGE_CALLER(), "SetLoadCallback"); g_realSer->SetLoadCallback(h, cb); }
		void ser_delete(std::uint32_t h, void* cb) { note_serialization(BRIDGE_CALLER(), "SetFormDeleteCallback"); g_realSer->SetFormDeleteCallback(h, cb); }

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

	void skse_proxy_init(const SKSEInterface* real, bool fanout)
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
		}
		logf("skse proxy: host handle %u, messaging %s (v%u), serialization %s", g_hostHandle,
			g_realMsg ? (fanout ? "fan-out" : "pass-through") : "unavailable", g_realMsg ? g_realMsg->interfaceVersion : 0,
			g_realSer ? "monitored" : "unavailable");
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
