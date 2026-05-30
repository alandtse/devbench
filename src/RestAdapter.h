#pragma once

namespace httplib
{
	class Server;
}

namespace dvb
{
	class ToolRegistry;
	class EventBus;

	/// Mounts a plain-HTTP REST facade (/api/*) onto an existing httplib server,
	/// sharing the MCP port. Generic — reflects the registry, no tool names. Lets
	/// non-MCP clients (curl, scripts, CI) reach the same tools without the JSON-RPC
	/// handshake. Mount before server.start().
	///
	///   GET  /api/tools            → registry descriptors (discovery + docs)
	///   POST /api/tool/<name>      → invoke; body is the arguments object
	///   GET  /api/events?since=N   → recent event ring (poll)
	class RestAdapter
	{
	public:
		RestAdapter(ToolRegistry& a_registry, EventBus& a_events);

		void Mount(httplib::Server& a_http);

	private:
		ToolRegistry& m_registry;
		EventBus& m_events;
	};
}
