#pragma once

#include <cstdint>

namespace mcp
{
	class server;
}

namespace dvb
{
	class ToolRegistry;
	class EventBus;

	/// Reflects a ToolRegistry onto an mcp::server — registers each tool as an MCP
	/// tool and forwards EventBus events as MCP notifications. Contains no individual
	/// tool name. Call Wire() after tools are registered and before server.start().
	class McpAdapter
	{
	public:
		McpAdapter(ToolRegistry& a_registry, EventBus& a_events, mcp::server& a_server);

		void Wire();

	private:
		ToolRegistry& m_registry;
		EventBus&     m_events;
		mcp::server&  m_server;
		uint64_t      m_sub = 0;
	};
}
