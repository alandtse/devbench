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

	// Live rebind (from the in-game FUCK menu): update the sink's key/shift atomically and persist
	// to config.json. shift = require Shift held; scancode 0 disables that hotkey.
	void SetRecordHotkey(int a_scancode, bool a_shift);
	void SetReplayHotkey(int a_scancode, bool a_shift);

	// Read the live hotkey binds (for the menu's read-only display / rebind seeding).
	void GetHotkeys(int& a_recordKey, bool& a_recordShift, int& a_replayKey, bool& a_replayShift);
}
