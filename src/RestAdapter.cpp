#include "RestAdapter.h"

#include "EventBus.h"
#include "Json.h"
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

	RestAdapter::RestAdapter(ToolRegistry& a_registry, EventBus& a_events) :
		m_registry(a_registry), m_events(a_events)
	{}

	void RestAdapter::Mount(httplib::Server& a_http)
	{
		// Discovery: descriptors double as documentation.
		a_http.Get("/api/tools", [this](const httplib::Request&, httplib::Response& res) {
			json arr = json::array();
			for (const auto& d : m_registry.List())
				arr.push_back(json{ { "name", d.name }, { "description", d.description },
					{ "inputSchema", d.inputSchema }, { "readOnly", d.readOnly } });
			WriteJson(res, 200, arr);
		});

		// Invoke: POST /api/tool/<name>, body is the arguments object.
		a_http.Post(R"(/api/tool/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
			const std::string name = req.matches[1];
			json args = json::object();
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
				arr.push_back(json{ { "seq", ev.seq }, { "topic", ev.topic }, { "data", ev.payload } });
			WriteJson(res, 200, json{ { "headSeq", m_events.HeadSeq() }, { "events", arr } });
		});
	}
}
