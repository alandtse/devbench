#pragma once

#include <string>

namespace dvb
{
	// Headless startup config (devbench has no in-game menu yet). Read once at
	// kDataLoaded, before the server starts. Bind address is intentionally fixed to
	// 127.0.0.1 and not configurable.
	struct Config
	{
		bool enabled = true;  ///< start the MCP/REST server at all
		int port = 8920;      ///< localhost port for /mcp and /api
	};

	// Load Data/SKSE/Plugins/devbench/config.json. Missing file or parse error →
	// defaults (logged). Never throws.
	Config LoadConfig();
}
