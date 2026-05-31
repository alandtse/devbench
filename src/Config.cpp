#include "Config.h"

#include "Json.h"

#include <fstream>

namespace dvb
{
	Config LoadConfig()
	{
		Config cfg;
		constexpr const char* path = "Data/SKSE/Plugins/devbench/config.json";

		std::ifstream file(path);
		if (!file.is_open()) {
			logs::info("devbench: no config at {} — defaults (enabled, port {})", path, cfg.port);
			return cfg;
		}
		try {
			json j;
			file >> j;
			cfg.enabled = j.value("enabled", cfg.enabled);
			cfg.port = j.value("port", cfg.port);
		} catch (const std::exception& e) {
			logs::warn("devbench: bad config ({}) — using defaults", e.what());
			return Config{};
		}
		logs::info("devbench: config enabled={} port={}", cfg.enabled, cfg.port);
		return cfg;
	}
}
