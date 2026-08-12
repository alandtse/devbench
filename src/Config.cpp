#include "Config.h"

#include "Json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

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
				{ "recordHotkeyShift", a_cfg.recordHotkeyShift },
				{ "replayHotkeyShift", a_cfg.replayHotkeyShift },
				{ "replayPath", a_cfg.replayPath },
				{ "replayRestoreScene", a_cfg.replayRestoreScene },
				{ "recordIntervalMs", a_cfg.recordIntervalMs },
				{ "autoRunPath", a_cfg.autoRunPath },
				{ "autoRunRestoreScene", a_cfg.autoRunRestoreScene },
				{ "loadSettleMs", a_cfg.loadSettleMs },
				{ "couplingAnchorMs", a_cfg.couplingAnchorMs },
				{ "couplingCellMs", a_cfg.couplingCellMs },
				{ "cleanTransition", a_cfg.cleanTransition },
				{ "cleanTransitionCell", a_cfg.cleanTransitionCell },
				{ "captureDir", a_cfg.captureDir },
				{ "captureScanDirs", a_cfg.captureScanDirs },
				{ "captureTimeoutMs", a_cfg.captureTimeoutMs },
				{ "captureSettleMs", a_cfg.captureSettleMs },
			};
			out << j.dump(2) << '\n';
		}
	}

	void SaveHotkeys(int a_recordKey, bool a_recordShift, int a_replayKey, bool a_replayShift)
	{
		const std::filesystem::path path = "Data/SKSE/Plugins/devbench/config.json";
		json                        j = json::object();
		if (std::ifstream in(path); in) {
			try {
				j = json::parse(in, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
			} catch (...) {
				j = json::object();  // corrupt file → still write a valid one with the new binds
			}
		}
		j["recordHotkey"] = a_recordKey;
		j["recordHotkeyShift"] = a_recordShift;
		j["replayHotkey"] = a_replayKey;
		j["replayHotkeyShift"] = a_replayShift;

		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);
		const std::filesystem::path tmp = path.string() + ".tmp";
		if (std::ofstream out(tmp, std::ios::trunc); out)
			out << j.dump(2) << '\n';
		else {
			logs::warn("devbench: could not write hotkeys to {}", tmp.string());
			return;
		}
		std::filesystem::rename(tmp, path, ec);  // atomic replace; no torn read
		if (ec)
			logs::warn("devbench: could not replace config.json ({})", ec.message());
	}

	Config LoadConfig()
	{
		Config cfg;
		// Deterministic default port per runtime so a fixed MCP client URL never moves: SE/AE 8920,
		// VR 8921. The two games run from separate Data dirs, so per-runtime ports never collide —
		// register both client entries (devbench-se :8920, devbench-vr :8921); the game that's off
		// just shows disconnected. An explicit "port" in config.json overrides this.
		cfg.port = REL::Module::IsVR() ? 8921 : 8920;
		const std::filesystem::path path = "Data/SKSE/Plugins/devbench/config.json";

		std::ifstream file(path);
		if (!file.is_open()) {
			WriteConfig(path, cfg);  // first run: leave a self-documenting template behind
			logs::info("devbench: no config at {} — wrote defaults (enabled, port {}, logLevel {})",
				path.string(), cfg.port, cfg.logLevel);
			return cfg;
		}
		try {
			// Allow // comments (the documented jsonc form) so a commented config still
			// parses instead of silently falling back to defaults.
			const json j = json::parse(file, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
			cfg.enabled = j.value("enabled", cfg.enabled);
			cfg.port = j.value("port", cfg.port);
			cfg.logLevel = j.value("logLevel", cfg.logLevel);
			cfg.recordHotkey = j.value("recordHotkey", cfg.recordHotkey);
			cfg.replayHotkey = j.value("replayHotkey", cfg.replayHotkey);
			cfg.recordHotkeyShift = j.value("recordHotkeyShift", cfg.recordHotkeyShift);
			cfg.replayHotkeyShift = j.value("replayHotkeyShift", cfg.replayHotkeyShift);
			cfg.replayPath = j.value("replayPath", cfg.replayPath);
			cfg.replayRestoreScene = j.value("replayRestoreScene", cfg.replayRestoreScene);
			cfg.recordIntervalMs = j.value("recordIntervalMs", cfg.recordIntervalMs);
			cfg.autoRunPath = j.value("autoRunPath", cfg.autoRunPath);
			cfg.autoRunRestoreScene = j.value("autoRunRestoreScene", cfg.autoRunRestoreScene);
			cfg.loadSettleMs = j.value("loadSettleMs", cfg.loadSettleMs);
			cfg.couplingAnchorMs = j.value("couplingAnchorMs", cfg.couplingAnchorMs);
			cfg.couplingCellMs = j.value("couplingCellMs", cfg.couplingCellMs);
			cfg.cleanTransition = j.value("cleanTransition", cfg.cleanTransition);
			cfg.cleanTransitionCell = j.value("cleanTransitionCell", cfg.cleanTransitionCell);
			cfg.captureDir = j.value("captureDir", cfg.captureDir);
			cfg.captureScanDirs = j.value("captureScanDirs", cfg.captureScanDirs);
			cfg.captureTimeoutMs = j.value("captureTimeoutMs", cfg.captureTimeoutMs);
			cfg.captureSettleMs = j.value("captureSettleMs", cfg.captureSettleMs);

			// Migrate forward: if the file predates any key (e.g. an install from before
			// the record hotkeys existed), rewrite it so the new keys appear with their
			// defaults — otherwise a user never discovers options added in an update. The
			// values just loaded above are preserved; only absent keys gain defaults.
			static constexpr const char* kKeys[] = {
				"enabled", "port", "logLevel", "recordHotkey", "replayHotkey",
				"recordHotkeyShift", "replayHotkeyShift", "replayPath", "replayRestoreScene",
				"recordIntervalMs", "autoRunPath", "autoRunRestoreScene", "loadSettleMs",
				"couplingAnchorMs", "couplingCellMs", "cleanTransition", "cleanTransitionCell",
				"captureDir", "captureScanDirs", "captureTimeoutMs", "captureSettleMs"
			};
			const bool complete = std::all_of(std::begin(kKeys), std::end(kKeys),
				[&](const char* k) { return j.contains(k); });
			if (!complete) {
				WriteConfig(path, cfg);
				logs::info("devbench: config migrated — added missing keys with defaults");
			}
		} catch (const std::exception& e) {
			logs::warn("devbench: bad config ({}) — using defaults", e.what());
			return Config{};
		}
		logs::info("devbench: config enabled={} port={} logLevel={}", cfg.enabled, cfg.port, cfg.logLevel);
		return cfg;
	}
}
