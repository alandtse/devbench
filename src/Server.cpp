#include "Server.h"

#include "McpAdapter.h"
#include "RestAdapter.h"
#include "Version.h"

#include <httplib.h>
#include <mcp_server.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <filesystem>
#include <fstream>

namespace
{
	// True if 127.0.0.1:port can be bound (i.e. it's free). WSAStartup is ref-counted,
	// so pairing it with WSACleanup here is safe whether or not winsock is already up.
	bool PortAvailable(const std::string& a_host, int a_port)
	{
		WSADATA      wsa;
		const bool   started = ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
		bool         available = true;
		const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s != INVALID_SOCKET) {
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = ::htons(static_cast<u_short>(a_port));
			::inet_pton(AF_INET, a_host.c_str(), &addr.sin_addr);
			available = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
			::closesocket(s);
		}
		if (started)
			::WSACleanup();
		return available;
	}

	// Publish the actually-bound port so fixed-URL clients can discover a non-default
	// choice (when auto-iteration moved off the configured port).
	void WriteRuntimeInfo(int a_port)
	{
		std::error_code ec;
		std::filesystem::create_directories("Data/SKSE/Plugins/devbench", ec);
		std::ofstream f("Data/SKSE/Plugins/devbench/runtime.json", std::ios::trunc);
		if (f)
			f << "{\"port\":" << a_port << "}\n";
	}
}

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

		// Find a free port starting at the configured one (a second instance or an
		// occupied port just moves to the next). The bound port is written to
		// runtime.json so fixed-URL clients can discover a non-default choice.
		constexpr int kMaxTries = 16;
		int           chosen = m_port;
		for (int i = 0; i < kMaxTries; ++i) {
			if (PortAvailable(m_host, m_port + i)) {
				chosen = m_port + i;
				break;
			}
		}

		// This cpp-mcp revision takes a configuration struct (host/port are no
		// longer positional ctor args).
		mcp::server::configuration cfg;
		cfg.host = m_host;
		cfg.port = chosen;
		cfg.name = "devbench";
		cfg.version = DEVBENCH_VERSION_STRING;

		m_mcp = std::make_unique<mcp::server>(cfg);
		m_mcp->set_server_info(cfg.name, cfg.version);
		// tools.listChanged: tools are added at runtime (cross-plugin consumers register kinds/menus),
		// so advertise the capability and emit notifications/tools/list_changed when the set changes —
		// a client that connected before a mod (or the game) finished loading then refreshes its list.
		m_mcp->set_capabilities(json{ { "tools", json{ { "listChanged", true } } }, { "logging", json::object() } });

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
		if (ok) {
			WriteRuntimeInfo(chosen);
			if (chosen != m_port)
				logs::info("devbench: configured port {} busy → bound {}", m_port, chosen);
		}
		logs::info("devbench: server on {}:{} — {}", m_host, chosen, ok ? "listening (mcp + rest)" : "FAILED to start");
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
