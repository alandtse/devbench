#include "Autorun.h"

#include "Json.h"
#include "ToolRegistry.h"

#include <atomic>
#include <thread>
#include <utility>

namespace dvb
{
	namespace
	{
		ToolRegistry*     g_registry = nullptr;
		std::string       g_path;
		bool              g_restore = true;
		std::atomic<bool> g_fired{ false };
	}

	void ArmAutoRun(ToolRegistry& a_registry, std::string a_path, bool a_restoreScene)
	{
		if (a_path.empty())
			return;
		g_registry = &a_registry;
		g_path = std::move(a_path);
		g_restore = a_restoreScene;
		g_fired.store(false);
		logs::info("devbench: autorun armed ({}, restoreScene={})", g_path, g_restore);
	}

	void OnPostLoadGame()
	{
		if (!g_registry)
			return;
		bool expected = false;
		if (!g_fired.compare_exchange_strong(expected, true))
			return;  // already fired this session — don't re-trigger on the replay's own reload

		// Off the main thread: the record/replay handler marshals back to the main thread via
		// RunAndWait, which would deadlock if invoked from the main-thread message handler.
		std::thread([]() {
			try {
				const json args{ { "action", "replay" }, { "path", g_path }, { "restoreScene", g_restore } };
				g_registry->Invoke("record", args, ToolContext{ "autorun" });
				logs::info("devbench: autorun replay complete");
			} catch (...) {
				logs::warn("devbench: autorun replay threw");
			}
		}).detach();
	}
}
