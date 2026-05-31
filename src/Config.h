#pragma once

#include <string>

namespace dvb
{
	// Headless startup config (devbench has no in-game menu yet). Read once at
	// kPostLoad, before the server starts. Bind address is intentionally fixed to
	// 127.0.0.1 and not configurable.
	struct Config
	{
		bool enabled = true;            ///< start the MCP/REST server at all
		int port = 8920;                ///< localhost port for /mcp and /api
		std::string logLevel = "info";  ///< trace|debug|info|warn|error (spdlog level)
	};

	// Load Data/SKSE/Plugins/devbench/config.json. If the file is missing it is
	// auto-created with the current defaults (so users/agents discover the keys); a
	// parse error → defaults (logged). Never throws.
	Config LoadConfig();
}
