#pragma once

#include <memory>
#include <string>

#include "EventBus.h"
#include "Json.h"
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
		explicit Server(std::string a_host = "127.0.0.1", int a_port = 8920);
		~Server();

		Server(const Server&) = delete;
		Server& operator=(const Server&) = delete;

		ToolRegistry& Tools() { return m_registry; }
		EventBus&     Events() { return m_events; }

		/// Wire adapters and start listening (non-blocking). Idempotent.
		bool Start();
		void Stop();
		bool Running() const;

	private:
		std::string                  m_host;
		int                          m_port;
		ToolRegistry                 m_registry;
		EventBus                     m_events;
		std::unique_ptr<mcp::server> m_mcp;
		std::unique_ptr<McpAdapter>  m_mcpAdapter;
		std::unique_ptr<RestAdapter> m_restAdapter;
	};

	/// The port actually bound by the (single, process-wide) Server instance — may differ
	/// from the configured port if it was busy, since Start() auto-iterates. 0 before a
	/// Server has started. Exposed so inspect{kind:"state"} can report it: with multiple
	/// game instances each on their own port, a caller can tell which one it's actually
	/// talking to instead of silently misattaching (devbench#16).
	int BoundPort();

	/// Basename of the running executable (SkyrimSE.exe / SkyrimVR.exe). Process-constant.
	std::string ExecutableName();

	/// { pid, port, exe, vr } identifying the instance that answered — computed off the main
	/// thread (pid/exe/vr cached once, port read live) so /api/health and inspect{kind:"state"}
	/// share one identity source and a caller can tell which game instance replied (devbench#16).
	json InstanceIdentity();

	/// { exePath, args, mcpJsonSnippet, installCommand } for the devbench-bridge companion
	/// (a separate MCP stdio proxy — see bridge/README.md — that survives this process
	/// restarting, unlike a client connected directly to this DLL's own /mcp endpoint).
	/// Surfaced at the top level of GET /api/tools (not inside any one tool's description,
	/// and not behind a separate endpoint) because that's the one place a cold agent
	/// doing REST discovery is guaranteed to already be looking.
	json BridgeDiscoveryInfo();

	/// Point RunTool at the process registry while a Server is up (set by Start, cleared by Stop),
	/// so the in-game menu — a separate render-thread TU with no Server reference — can invoke tools.
	void SetProcessRegistry(ToolRegistry* a_registry);

	/// Invoke a devbench tool by name from anywhere (e.g. the SMF menu). Returns the tool's value,
	/// or { error, code } on failure / when no Server is up. Only call fast, non-blocking tools from
	/// the render thread (recordings; record replay is async by default) — a RunAndWait-backed tool
	/// would stall the caller.
	json RunTool(const std::string& a_name, const json& a_args);

	/// The `recordings` list, cached and refreshed at most ~once/second. A menu redraws every frame,
	/// and the underlying list fully parses every recording file — calling it per frame would stall
	/// the render thread. Call InvalidateRecordingsCache() after a validate/delete to force a refresh.
	json ListRecordingsCached();
	void InvalidateRecordingsCache();

	/// Open the recordings directory in Explorer (menu "Open folder"); OpenRecordingFile selects one
	/// file. No-op if the directory is unknown. Not an ImGui call, so it lives in the main TU.
	void OpenRecordingsFolder();
	void OpenRecordingFile(const std::string& a_file);
}
