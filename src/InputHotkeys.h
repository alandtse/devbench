#pragma once

namespace dvb
{
	class ToolRegistry;
	struct Config;

	// Register an input sink for the config-driven record/replay hotkeys, so recording and
	// replay can be driven in-game with NO MCP/REST client connected (the standalone-benchmark
	// path). No-op if neither hotkey is set. The hotkeys reuse the registered `record` tool,
	// so they share exactly the API/MCP code path.
	void InstallInputHotkeys(ToolRegistry& a_registry, const Config& a_config);
}
