#include "Autorun.h"
#include "Config.h"
#include "ConsoleHook.h"
#include "GameEvents.h"
#include "GameState.h"
#include "HostApi.h"
#include "InputHotkeys.h"
#include "Recording.h"
#include "Server.h"
#include "Tools.h"
#include "Version.h"

#include <cstring>
#include <memory>
#include <spdlog/sinks/basic_file_sink.h>

namespace
{
	// Constructed at kDataLoaded with the configured port (null if disabled via config).
	std::unique_ptr<dvb::Server> g_server;
	dvb::Config                  g_config;  // captured at kPostLoad; used at kInputLoaded

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

	// Apply the config's log level (default info). Lowering to debug/trace surfaces
	// per-tool-invocation logging for diagnosing client/agent interactions.
	void ApplyLogLevel(const std::string& a_level)
	{
		const auto lvl = spdlog::level::from_str(a_level);  // unknown string → 'off'; guard that
		const auto resolved = (lvl == spdlog::level::off && a_level != "off") ? spdlog::level::info : lvl;
		if (resolved == spdlog::level::off && a_level != "off")
			logs::warn("devbench: unknown logLevel '{}' — keeping info", a_level);
		spdlog::set_level(resolved);
		spdlog::flush_on(resolved);
	}

	// Listener for messages from ANY plugin (registered with a nullptr sender). The
	// default OnMessage listener only receives SKSE-sender messages, so a consumer mod's
	// interface request would never reach us without this. Only acts on the request.
	void OnInterfaceMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		dvb::HostApi::OnInterfaceRequest(a_msg);
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg)
			return;
		// Init at kPostLoad, not kDataLoaded: SKSE runs ALL plugins' kPostLoad before any
		// kDataLoaded, so the cross-plugin interface is ready when consumer mods request it
		// at their kDataLoaded (otherwise plugin order can make us answer too late — a
		// consumer would see "devbench not present"). The HTTP server only listens here;
		// tool handlers query game state lazily when invoked (by then the game is loaded).
		if (a_msg->type == SKSE::MessagingInterface::kPostLoad) {
			const dvb::Config cfg = dvb::LoadConfig();
			ApplyLogLevel(cfg.logLevel);
			if (!cfg.enabled) {
				logs::info("devbench: server disabled via config; not starting");
			} else {
				// Register built-in tools and wire the cross-plugin host API (which
				// registers its self-test tool) BEFORE Start() so they appear on both
				// transports from the first request; then attach game-event sources.
				g_config = cfg;  // kept for kInputLoaded (input sink registers later)
				g_server = std::make_unique<dvb::Server>("127.0.0.1", cfg.port);
				g_server->Events().SetFrameProvider(&dvb::game::CurrentFrame);
				dvb::RegisterCoreTools(g_server->Tools(), g_server->Events());
				dvb::Recording::SetLoadSettleMs(cfg.loadSettleMs);
				dvb::Recording::SetDefaultIntervalMs(cfg.recordIntervalMs);
				dvb::Recording::SetCoupling(cfg.couplingAnchorMs, cfg.couplingCellMs, cfg.cleanTransition, cfg.cleanTransitionCell);
				dvb::ArmAutoRun(g_server->Tools(), cfg.autoRunPath, cfg.autoRunRestoreScene);
				dvb::HostApi::Init(g_server->Tools(), g_server->Events());
				g_server->Start();
				dvb::InstallGameEvents(g_server->Events());
				dvb::ConsoleHook::Install(g_server->Events());  // observe console commands as events / for recording

				// Receive cross-plugin interface requests from ANY plugin (nullptr sender),
				// so consumer mods' dispatches reach us (mirrors MergeMapper). Registered
				// at kPostLoad, before consumers request at their kDataLoaded.
				if (auto* m = SKSE::GetMessagingInterface())
					m->RegisterListener(nullptr, OnInterfaceMessage);
			}
		}
		// Register input hotkeys at kInputLoaded — BSInputDeviceManager is null at kPostLoad,
		// so registering then silently no-ops. kInputLoaded fires once the input subsystem is up.
		if (a_msg->type == SKSE::MessagingInterface::kInputLoaded && g_server)
			dvb::InstallInputHotkeys(g_server->Tools(), g_config);

		if (g_server) {
			// Publish lifecycle events (dataLoaded and later load/save/new-game).
			dvb::OnSKSEMessage(a_msg->type);
			// Remember the save the player loaded or just wrote, so a recording started
			// afterward can stamp a reproducible entry point — even when the user loaded via
			// the in-game menu (devbench didn't broker it). kPreLoadGame / kSaveGame carry the
			// save's base name as a null-terminated string in data.
			//
			// Read it with a BOUNDED strnlen — do NOT use a_msg->dataLen. For kSaveGame dataLen is
			// not the string length, and std::string(data, dataLen) with a bogus length allocates
			// huge and HANGS the main thread (this handler runs there). Verified live: that hang
			// is exactly what froze the game on a console `save`.
			if ((a_msg->type == SKSE::MessagingInterface::kPreLoadGame ||
					a_msg->type == SKSE::MessagingInterface::kSaveGame) &&
				a_msg->data) {
				const char* raw = static_cast<const char*>(a_msg->data);
				std::string name(raw, strnlen(raw, 255));  // capped; never trust dataLen
				if (name.size() > 4 && name.compare(name.size() - 4, 4, ".ess") == 0)
					name.resize(name.size() - 4);  // the load tool takes the stem, not the filename
				if (!name.empty())
					dvb::Recording::NoteLoadEntry(name);
			}
			// Fire the one-shot autorun benchmark once a game has loaded.
			if (a_msg->type == SKSE::MessagingInterface::kPostLoadGame)
				dvb::OnPostLoadGame();
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	InitLogging();
	SKSE::AllocTrampoline(1 << 10);
	logs::info("devbench {} loaded", DEVBENCH_VERSION_STRING);

	if (auto* messaging = SKSE::GetMessagingInterface())
		messaging->RegisterListener(OnMessage);

	return true;
}
