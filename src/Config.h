#pragma once

#include <string>

namespace dvb
{
	// Headless startup config (devbench has no in-game menu yet). Read once at
	// kPostLoad, before the server starts. Bind address is intentionally fixed to
	// 127.0.0.1 and not configurable.
	struct Config
	{
		bool        enabled = true;     ///< start the MCP/REST server at all
		int         port = 8920;        ///< localhost port for /mcp and /api (default SE/AE 8920, VR 8921 — LoadConfig)
		std::string logLevel = "info";  ///< trace|debug|info|warn|error (spdlog level)

		// In-game hotkeys (DXScanCode; 0 = disabled). Let recording/replay run with no
		// MCP/REST client connected — the standalone-benchmark path. Ignored while the
		// console is open so they don't fire on keystrokes typed into it.
		int         recordHotkey = 0;           ///< toggle record start/stop (DXScanCode)
		int         replayHotkey = 0;           ///< replay a recording (see replayPath)
		bool        recordHotkeyShift = false;  ///< require Shift held with recordHotkey
		bool        replayHotkeyShift = false;  ///< require Shift held with replayHotkey
		std::string replayPath = "";            ///< replay target; empty = most recent recording
		bool        replayRestoreScene = true;  ///< replay hotkey re-establishes the recorded scene
		int         recordIntervalMs = 10;      ///< default record sample interval (ms; min 10); per-call intervalMs overrides

		// Autorun: replay a recording once on the first load of the session — a fully
		// unattended benchmark with no client and no keypress. Empty = off.
		std::string autoRunPath = "";            ///< recording to replay on first postLoadGame
		bool        autoRunRestoreScene = true;  ///< autorun loads the recording's entry save first

		// Settle delay (ms) inserted after a restore-load before the replayed trajectory runs.
		// Teleporting the player the instant a load finishes (cells/physics/AI not settled) is
		// crash-prone. This is LOCAL/per-machine (settle time is hardware-dependent), not baked
		// into the portable recording. A replay call may override it with a settleMs arg.
		int loadSettleMs = 3000;

		// Scene coupling: how strictly a replay must reproduce the recorded entry, chosen by
		// how long before record-start devbench brokered the save/coc (stored per recipe as
		// entryPoint.ageMs). A save staged seconds before recording was deliberate; one from
		// minutes ago was incidental. Tiers (a recipe may override in its meta.coupling block):
		//   age <= anchorMs : "anchored" — restore the entry + re-apply recorded time/weather.
		//   age <= cellMs    : "cell"     — restore the entry.
		//   else             : "worldspace" — skip the entry restore; only assert the worldspace.
		int couplingAnchorMs = 10000;
		int couplingCellMs = 60000;

		// A raw coc/cow can stream between scenes without the full loading-screen teardown some
		// mods rely on to free resources, which can CTD. When true, a coc/cow restore first
		// bounces through couplingTransitionCell (a known-present interior) to force a clean
		// loading screen. Save-loads already tear down, so they skip the bounce.
		bool        cleanTransition = true;
		std::string cleanTransitionCell = "QASmoke";
	};

	// Load Data/SKSE/Plugins/devbench/config.json. If the file is missing it is
	// auto-created with the current defaults (so users/agents discover the keys); a
	// parse error → defaults (logged). Never throws.
	Config LoadConfig();
}
