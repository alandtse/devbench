#include "Config.h"
#include "ConsoleLogCapture.h"
#include "GameEvents.h"
#include "HostApi.h"
#include "Server.h"
#include "Tools.h"
#include "Version.h"

#include <memory>
#include <spdlog/sinks/basic_file_sink.h>

namespace
{
	// Constructed at kDataLoaded with the configured port (null if disabled via config).
	std::unique_ptr<dvb::Server> g_server;

	void InitLogging()
	{
		auto path = SKSE::log::log_directory();
		if (!path)
			return;
		*path /= std::format("{}.log", SKSE::PluginDeclaration::GetSingleton()->GetName());

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%^%L%$] %v");
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		// The server is started after data load so game state is queryable. Wiring
		// (ToolRegistry + MCP/REST adapters over cpp-mcp's httplib) lands next.
		if (!a_msg)
			return;
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			const dvb::Config cfg = dvb::LoadConfig();
			if (!cfg.enabled) {
				logs::info("devbench: server disabled via config; not starting");
			} else {
				// Install the console detour, register built-in tools, and wire the
				// cross-plugin host API (which registers its self-test tool) all BEFORE
				// Start() so they appear on both transports from the first request; then
				// attach game-event sources.
				dvb::ConsoleLogCapture::Install();
				g_server = std::make_unique<dvb::Server>("127.0.0.1", cfg.port);
				dvb::RegisterCoreTools(g_server->Tools());
				dvb::HostApi::Init(g_server->Tools(), g_server->Events());
				g_server->Start();
				dvb::InstallGameEvents(g_server->Events());
			}
		}
		if (g_server) {
			// Cross-plugin interface handshake (no-op unless it's the request message).
			dvb::HostApi::OnInterfaceRequest(a_msg);
			// Publish lifecycle events (dataLoaded and later load/save/new-game).
			dvb::OnSKSEMessage(a_msg->type);
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	InitLogging();
	SKSE::AllocTrampoline(64);  // for the ConsoleLogCapture VPrint detour
	logs::info("devbench {} loaded", DEVBENCH_VERSION_STRING);

	if (auto* messaging = SKSE::GetMessagingInterface())
		messaging->RegisterListener(OnMessage);

	return true;
}
