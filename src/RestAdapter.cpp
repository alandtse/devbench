#include "RestAdapter.h"

#include "EventBus.h"
#include "GameState.h"  // game::CurrentFrame
#include "Json.h"
#include "MainThread.h"  // LastCompletedFrame / PendingTasks
#include "Server.h"      // InstanceIdentity
#include "ToolRegistry.h"

#include <httplib.h>

namespace dvb
{
	namespace
	{
		void WriteJson(httplib::Response& a_res, int a_status, const json& a_body)
		{
			a_res.status = a_status;
			a_res.set_content(a_body.dump(), "application/json");
		}
	}

	namespace
	{
		// A malformed lifecycle payload must never clear an already-known-good value —
		// skip it rather than let .value() throw or silently store an empty string.
		std::optional<std::string> LifecycleEventName(const json& a_payload)
		{
			if (!a_payload.is_object())
				return std::nullopt;
			const auto it = a_payload.find("event");
			if (it == a_payload.end() || !it->is_string())
				return std::nullopt;
			return it->get<std::string>();
		}
	}

	RestAdapter::RestAdapter(ToolRegistry& a_registry, EventBus& a_events) :
		m_registry(a_registry), m_events(a_events)
	{
		// Track the latest lifecycle event directly rather than scanning Since(0) on every
		// /api/health call: the bus's ring buffer caps at 256 events, so under enough traffic
		// a real lifecycle event scrolls out and health would wrongly report null even though
		// one had occurred. Subscribe() only delivers FUTURE events, so also seed from
		// Since(0) once here — today's call order (Start() before InstallGameEvents) means no
		// lifecycle event can precede this subscription, but seeding removes that as a silent
		// invariant this constructor depends on.
		for (const auto& ev : m_events.Since(0))
			if (ev.topic == "lifecycle")
				if (auto name = LifecycleEventName(ev.payload))
					m_lastLifecycle = std::move(name);

		m_lifecycleSub = m_events.Subscribe([this](const EventBus::Event& a_ev) {
			if (a_ev.topic != "lifecycle")
				return;
			auto name = LifecycleEventName(a_ev.payload);
			if (!name)
				return;
			std::lock_guard lock(m_lifecycleMtx);
			m_lastLifecycle = std::move(name);
		});
	}

	RestAdapter::~RestAdapter()
	{
		if (m_lifecycleSub)
			m_events.Unsubscribe(m_lifecycleSub);
	}

	void RestAdapter::Mount(httplib::Server& a_http)
	{
		// Discovery: descriptors double as documentation. `mcp_bridge` sits at the top
		// level (not inside a tool's own description) because this is the first call any
		// REST-discovering agent makes — see BridgeDiscoveryInfo()'s doc comment for why
		// placement here, specifically, is what makes it actually get noticed.
		a_http.Get("/api/tools", [this](const httplib::Request&, httplib::Response& res) {
			json arr = json::array();
			for (const auto& d : m_registry.List())
				arr.push_back(json{ { "name", d.name }, { "description", d.description },
					{ "inputSchema", d.inputSchema }, { "readOnly", d.readOnly } });
			WriteJson(res, 200, json{ { "tools", std::move(arr) }, { "mcp_bridge", BridgeDiscoveryInfo() } });
		});

		// Invoke: POST /api/tool/<name>, body is the arguments object.
		a_http.Post(R"(/api/tool/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
			const std::string name = req.matches[1];
			json              args = json::object();
			if (!req.body.empty()) {
				try {
					args = json::parse(req.body);
				} catch (const std::exception& e) {
					WriteJson(res, 400, json{ { "error", std::string("invalid JSON body: ") + e.what() } });
					return;
				}
			}
			const ToolResult r = m_registry.Invoke(name, args, ToolContext{ /*clientId*/ "" });
			if (r.ok)
				WriteJson(res, 200, r.value);
			else
				WriteJson(res, r.errorCode ? r.errorCode : 500, json{ { "error", r.errorMessage }, { "code", r.errorCode } });
		});

		// Deliberately GET, not POST (a bodyless POST stalls on httplib's read timeout; see
		// Server::Start). Answered on the listener thread — no RunAndWait — so it keeps
		// replying while the main thread is busy (#56) and shows which instance replied (#16).
		// Reading the fields (busy vs hung, frame < 0, etc.): see the README.
		a_http.Get("/api/health", [this](const httplib::Request&, httplib::Response& res) {
			std::optional<std::string> lifecycle;
			{
				std::lock_guard lock(m_lifecycleMtx);
				lifecycle = m_lastLifecycle;
			}
			json body{
				{ "ok", true },
				{ "lastLifecycle", lifecycle ? json(*lifecycle) : json(nullptr) },
				{ "frame", game::CurrentFrame() },
				{ "lastTaskFrame", MainThread::LastCompletedFrame() },
				{ "pendingTasks", MainThread::PendingTasks() },
			};
			body.update(InstanceIdentity());  // pid, port, exe, vr
			WriteJson(res, 200, body);
		});

		// Poll recent events (the SSE stream is a later addition).
		a_http.Get("/api/events", [this](const httplib::Request& req, httplib::Response& res) {
			uint64_t since = 0;
			if (req.has_param("since")) {
				try {
					since = std::stoull(req.get_param_value("since"));
				} catch (...) {
				}
			}
			json arr = json::array();
			for (const auto& ev : m_events.Since(since))
				arr.push_back(json{ { "seq", ev.seq }, { "frame", ev.frame }, { "topic", ev.topic }, { "data", ev.payload } });
			WriteJson(res, 200, json{ { "headSeq", m_events.HeadSeq() }, { "events", arr } });
		});
	}
}
