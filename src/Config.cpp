#include "Config.h"

#include "Json.h"

#include <filesystem>
#include <fstream>

namespace dvb
{
	namespace
	{
		// Write the config as JSON so a fresh install ships a self-documenting file the
		// user/agent can edit, rather than having to know the keys exist.
		void WriteConfig(const std::filesystem::path& a_path, const Config& a_cfg)
		{
			std::error_code ec;
			std::filesystem::create_directories(a_path.parent_path(), ec);
			std::ofstream out(a_path, std::ios::trunc);
			if (!out) {
				logs::warn("devbench: could not write default config to {}", a_path.string());
				return;
			}
			const json j{
				{ "enabled", a_cfg.enabled },
				{ "port", a_cfg.port },
				{ "logLevel", a_cfg.logLevel },
				{ "recordHotkey", a_cfg.recordHotkey },
				{ "replayHotkey", a_cfg.replayHotkey },
				{ "replayPath", a_cfg.replayPath },
				{ "replayRestoreScene", a_cfg.replayRestoreScene },
				{ "autoRunPath", a_cfg.autoRunPath },
				{ "autoRunRestoreScene", a_cfg.autoRunRestoreScene },
				{ "loadSettleMs", a_cfg.loadSettleMs },
			};
			out << j.dump(2) << '\n';
		}
	}

	Config LoadConfig()
	{
		Config                      cfg;
		const std::filesystem::path path = "Data/SKSE/Plugins/devbench/config.json";

		std::ifstream file(path);
		if (!file.is_open()) {
			WriteConfig(path, cfg);  // first run: leave a self-documenting template behind
			logs::info("devbench: no config at {} — wrote defaults (enabled, port {}, logLevel {})",
				path.string(), cfg.port, cfg.logLevel);
			return cfg;
		}
		try {
			json j;
			file >> j;
			cfg.enabled = j.value("enabled", cfg.enabled);
			cfg.port = j.value("port", cfg.port);
			cfg.logLevel = j.value("logLevel", cfg.logLevel);
			cfg.recordHotkey = j.value("recordHotkey", cfg.recordHotkey);
			cfg.replayHotkey = j.value("replayHotkey", cfg.replayHotkey);
			cfg.replayPath = j.value("replayPath", cfg.replayPath);
			cfg.replayRestoreScene = j.value("replayRestoreScene", cfg.replayRestoreScene);
			cfg.autoRunPath = j.value("autoRunPath", cfg.autoRunPath);
			cfg.autoRunRestoreScene = j.value("autoRunRestoreScene", cfg.autoRunRestoreScene);
			cfg.loadSettleMs = j.value("loadSettleMs", cfg.loadSettleMs);
		} catch (const std::exception& e) {
			logs::warn("devbench: bad config ({}) — using defaults", e.what());
			return Config{};
		}
		logs::info("devbench: config enabled={} port={} logLevel={}", cfg.enabled, cfg.port, cfg.logLevel);
		return cfg;
	}
}
