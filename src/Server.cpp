#include "Server.h"

#include "McpAdapter.h"
#include "RestAdapter.h"
#include "Version.h"

#include <httplib.h>
#include <mcp_server.h>

namespace dvb
{
	Server::Server(std::string a_host, int a_port) :
		m_host(std::move(a_host)), m_port(a_port)
	{}

	Server::~Server()
	{
		Stop();
	}

	bool Server::Start()
	{
		if (m_mcp)
			return true;

		// This cpp-mcp revision takes a configuration struct (host/port are no
		// longer positional ctor args).
		mcp::server::configuration cfg;
		cfg.host = m_host;
		cfg.port = m_port;
		cfg.name = "devbench";
		cfg.version = DEVBENCH_VERSION_STRING;

		m_mcp = std::make_unique<mcp::server>(cfg);
		m_mcp->set_server_info(cfg.name, cfg.version);
		m_mcp->set_capabilities(json{ { "tools", json::object() }, { "logging", json::object() } });

		// MCP tools + notifications.
		m_mcpAdapter = std::make_unique<McpAdapter>(m_registry, m_events, *m_mcp);
		m_mcpAdapter->Wire();

		// REST facade on the same httplib server (constructed in mcp::server's ctor,
		// so http() is valid here; cpp-mcp adds its own routes during start()).
		m_restAdapter = std::make_unique<RestAdapter>(m_registry, m_events);
		if (auto* http = m_mcp->http())
			m_restAdapter->Mount(*http);
		else
			logs::warn("devbench: cpp-mcp http() returned null; REST facade unavailable");

		const bool ok = m_mcp->start(false);  // non-blocking; spawns the listener thread
		logs::info("devbench: server on {}:{} — {}", m_host, m_port, ok ? "listening (mcp + rest)" : "FAILED to start");
		return ok;
	}

	void Server::Stop()
	{
		if (m_mcp) {
			m_mcp->stop();
			m_mcp.reset();
		}
		m_restAdapter.reset();
		m_mcpAdapter.reset();
	}

	bool Server::Running() const
	{
		return m_mcp && m_mcp->is_running();
	}
}
