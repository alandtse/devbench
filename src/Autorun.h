#pragma once

#include <string>

namespace dvb
{
	class ToolRegistry;

	// Arm a one-shot replay of a recording on the first postLoadGame of the session, so a
	// benchmark runs with NO MCP/REST client and no keypress (the unattended-benchmark path).
	// No-op if the path is empty.
	void ArmAutoRun(ToolRegistry& a_registry, std::string a_path, bool a_restoreScene);

	// Call on kPostLoadGame. Fires the armed replay exactly once per session, then disarms —
	// so the replay's own save-reload (restoreScene) doesn't re-trigger it.
	void OnPostLoadGame();
}
