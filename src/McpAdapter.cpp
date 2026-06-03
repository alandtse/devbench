#include "McpAdapter.h"

#include "EventBus.h"
#include "McpContent.h"
#include "ToolRegistry.h"

#include <mcp_server.h>
#include <mcp_tool.h>

namespace dvb
{
	McpAdapter::McpAdapter(ToolRegistry& a_registry, EventBus& a_events, mcp::server& a_server) :
		m_registry(a_registry), m_events(a_events), m_server(a_server)
	{}

	McpAdapter::~McpAdapter()
	{
		if (m_sub)
			m_events.Unsubscribe(m_sub);  // bus worker won't call our callback after this returns
	}

	void McpAdapter::Wire()
	{
		// Generic registration — forwards to the registry by name, so no tool-specific
		// code lives in the adapter. Used for both existing tools and (via the registry's
		// registration listener) tools added later by cross-plugin consumers at kPostLoad.
		auto registerOne = [this](const ToolDescriptor& a_desc) {
			mcp::tool t;
			t.name = a_desc.name;
			t.description = a_desc.description;
			t.parameters_schema = a_desc.inputSchema;

			m_server.register_tool(t, [this, name = a_desc.name](const json& a_args, const std::string& a_session) -> json {
				const ToolResult r = m_registry.Invoke(name, a_args, ToolContext{ a_session });
				if (r.ok)
					return ToContentBlocks(r.value);  // must be a content ARRAY — see McpContent.h
				// Throwing lets cpp-mcp build the canonical error result (isError:true +
				// text content array); returning an {isError,...} object here would land
				// inside "content" as a non-array and break the same validation. cpp-mcp
				// only surfaces what(), so fold the status code into the message.
				throw std::runtime_error("[" + std::to_string(r.errorCode) + "] " + r.errorMessage);
			});
		};

		for (const auto& desc : m_registry.List())
			registerOne(desc);

		// Expose tools registered after wiring (cross-plugin consumers). Note: this can
		// call register_tool from the main/message thread post-start; safe in practice
		// because consumers register at kPostLoad, before any MCP client connects.
		m_registry.SetRegistrationListener(registerOne);

		// Forward bus events to connected MCP clients as notifications.
		m_sub = m_events.Subscribe([this](const EventBus::Event& a_ev) {
			m_server.broadcast_notification(mcp::request::create_notification(
				"notifications/message",
				json{ { "topic", a_ev.topic }, { "seq", a_ev.seq }, { "data", a_ev.payload } }));
		});
	}
}
