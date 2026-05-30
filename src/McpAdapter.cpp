#include "McpAdapter.h"

#include "EventBus.h"
#include "ToolRegistry.h"

#include <mcp_server.h>
#include <mcp_tool.h>

namespace dvb
{
	McpAdapter::McpAdapter(ToolRegistry& a_registry, EventBus& a_events, mcp::server& a_server) :
		m_registry(a_registry), m_events(a_events), m_server(a_server)
	{}

	void McpAdapter::Wire()
	{
		// Register every current tool. The handler is generic — it forwards to the
		// registry by name, so no tool-specific code lives in the adapter.
		for (const auto& desc : m_registry.List()) {
			mcp::tool t;
			t.name = desc.name;
			t.description = desc.description;
			t.parameters_schema = desc.inputSchema;

			m_server.register_tool(t, [this, name = desc.name](const json& a_args, const std::string& a_session) -> json {
				const ToolResult r = m_registry.Invoke(name, a_args, ToolContext{ a_session });
				if (r.ok)
					return r.value;
				// Surface a failure as an MCP tool error result (isError + text).
				return json{
					{ "isError", true },
					{ "code", r.errorCode },
					{ "content", json::array({ json{ { "type", "text" }, { "text", r.errorMessage } } }) },
				};
			});
		}

		// Forward bus events to connected MCP clients as notifications.
		m_sub = m_events.Subscribe([this](const EventBus::Event& a_ev) {
			m_server.broadcast_notification(mcp::request::create_notification(
				"notifications/message",
				json{ { "topic", a_ev.topic }, { "seq", a_ev.seq }, { "data", a_ev.payload } }));
		});
	}
}
