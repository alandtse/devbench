#include "Version.h"

#include <spdlog/sinks/basic_file_sink.h>

namespace
{
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
		if (a_msg && a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			logs::info("devbench: data loaded");
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	InitLogging();
	logs::info("devbench {} loaded", DEVBENCH_VERSION_STRING);

	if (auto* messaging = SKSE::GetMessagingInterface())
		messaging->RegisterListener(OnMessage);

	return true;
}
