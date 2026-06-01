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

		// Unsubscribe from the bus before we're destroyed — the bus worker thread (which outlives
		// this adapter: Server destroys the adapter before the EventBus) must not call our
		// subscriber after `this`/the server reference are gone. Unsubscribe barriers on any
		// in-flight delivery, so this is a clean cutoff.
		~McpAdapter();

		void Wire();

	private:
		ToolRegistry& m_registry;
		EventBus&     m_events;
		mcp::server&  m_server;
		uint64_t      m_sub = 0;
	};
}
