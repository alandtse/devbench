#include "Server.h"

#include "McpAdapter.h"
#include "RestAdapter.h"
#include "Version.h"

#include <RE/Skyrim.h>  // REL::Module::IsVR
#include <httplib.h>
#include <mcp_server.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <filesystem>
#include <fstream>

namespace
{
	std::atomic<int> g_boundPort{ 0 };

	// Process-wide registry pointer so dvb::RunTool can invoke tools from the in-game menu (a
	// separate render-thread TU) without a Server reference. Set/cleared by Server::Start/Stop.
	dvb::ToolRegistry* g_registry = nullptr;

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
		if (auto* http = m_mcp->http()) {
			// httplib's default 5s read timeout applies even to a bodyless POST (no
			// Content-Length/chunked header) — it waits for a body that will never come
			// before giving up with an empty 400. Shorten this so that mistake fails fast
			// instead of stalling; GET /api/health is the real fix for liveness checks.
			http->set_read_timeout(2, 0);
			m_restAdapter->Mount(*http);
		} else
			logs::warn("devbench: cpp-mcp http() returned null; REST facade unavailable");

		// Publish the chosen port before start() spawns the listener, so a health/inspect
		// hit racing startup reads the right port rather than 0.
		g_boundPort.store(chosen);
		g_registry = &m_registry;             // reachable by dvb::RunTool (the in-game menu) while up
		const bool ok = m_mcp->start(false);  // non-blocking; spawns the listener thread
		if (ok) {
			WriteRuntimeInfo(chosen);
			if (chosen != m_port)
				logs::info("devbench: configured port {} busy → bound {}", m_port, chosen);
		} else {
			// Tear down the constructed-but-not-listening members: the `if (m_mcp)` guard at
			// the top treats a non-null m_mcp as "already started", so leaving them set would
			// make a later Start() return true without a live listener. Reset the port too, or
			// it would advertise a live bridge for a server that never came up.
			g_boundPort.store(0);
			g_registry = nullptr;
			m_restAdapter.reset();
			m_mcpAdapter.reset();
			m_mcp.reset();
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
		g_boundPort.store(0);
		g_registry = nullptr;
	}

	bool Server::Running() const
	{
		return m_mcp && m_mcp->is_running();
	}

	int BoundPort()
	{
		return g_boundPort.load();
	}

	void SetProcessRegistry(ToolRegistry* a_registry)
	{
		g_registry = a_registry;
	}

	json RunTool(const std::string& a_name, const json& a_args)
	{
		if (!g_registry)
			return json{ { "error", "devbench server not running" }, { "code", 503 } };
		ToolContext ctx{ "ui" };
		ctx.internal = true;  // menu-driven; don't log each call
		const ToolResult r = g_registry->Invoke(a_name, a_args, ctx);
		return r.ok ? r.value : json{ { "error", r.errorMessage }, { "code", r.errorCode } };
	}

	std::string ExecutableName()
	{
		char        path[MAX_PATH]{};
		const DWORD len = ::GetModuleFileNameA(nullptr, path, MAX_PATH);
		if (len == 0 || len == MAX_PATH)
			return {};
		return std::filesystem::path(path).filename().string();
	}

	json InstanceIdentity()
	{
		// pid/exe/vr are constant for the process lifetime — compute once. /api/health is
		// polled frequently, so avoid a GetModuleFileNameA + path + string alloc per call;
		// only the bound port (an atomic) is read live.
		static const int         pid = static_cast<int>(::GetCurrentProcessId());
		static const std::string exe = ExecutableName();
		static const bool        vr = REL::Module::IsVR();
		return json{ { "pid", pid }, { "port", BoundPort() }, { "exe", exe }, { "vr", vr } };
	}
}
