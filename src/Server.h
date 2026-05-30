#pragma once

#include <memory>
#include <string>

#include "EventBus.h"
#include "ToolRegistry.h"

namespace mcp
{
	class server;
}

namespace dvb
{
	class McpAdapter;
	class RestAdapter;

	/// Owns the registry, event bus, and the cpp-mcp server, and mounts both the MCP
	/// and REST adapters on the one httplib instance (one localhost port). Register
	/// tools via Tools() before Start().
	class Server
	{
	public:
		explicit Server(std::string a_host = "127.0.0.1", int a_port = 8910);
		~Server();

		Server(const Server&) = delete;
		Server& operator=(const Server&) = delete;

		ToolRegistry& Tools() { return m_registry; }
		EventBus& Events() { return m_events; }

		/// Wire adapters and start listening (non-blocking). Idempotent.
		bool Start();
		void Stop();
		bool Running() const;

	private:
		std::string m_host;
		int m_port;
		ToolRegistry m_registry;
		EventBus m_events;
		std::unique_ptr<mcp::server> m_mcp;
		std::unique_ptr<McpAdapter> m_mcpAdapter;
		std::unique_ptr<RestAdapter> m_restAdapter;
	};
}
