#include "ConsoleHook.h"

#include "ConsoleLogCapture.h"
#include "EventBus.h"
#include "Json.h"
#include "Recording.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <cstring>

namespace dvb::ConsoleHook
{
	namespace
	{
		EventBus* g_events = nullptr;
		bool      g_installed = false;

		// The console command FxDelegate executor — RELOCATION_ID(50157, 51084). Both the in-game
		// console UI (on Enter) and RE::Console::ExecuteCommand funnel through here, so a context
		// hook at its entry observes every command. The command text is the first GFx arg.
		constexpr int kPatchSize = 8;

		void Detour(CONTEXT& a_ctx)
		{
			// An observer error must never escape into the game's command path, so swallow
			// everything and always fall through to the original executor.
			try {
				const auto* a_args = reinterpret_cast<RE::FxDelegateArgs*>(a_ctx.Rcx);
				if (a_args && a_args->GetArgCount() >= 1) {
					const RE::GFxValue& v = (*a_args)[0];
					if (v.IsString()) {
						const char* cmd = v.GetString();
						// Skip devbench's own capture fences (ConsoleLogCapture) so they don't
						// pollute events or recordings.
						if (cmd && *cmd &&
							std::strcmp(cmd, ConsoleLogCapture::kMarkerBegin) != 0 &&
							std::strcmp(cmd, ConsoleLogCapture::kMarkerEnd) != 0) {
							if (g_events)
								g_events->Publish("console.command", json{ { "command", cmd } });
							Recording::NoteConsoleCommand(cmd);
						}
					}
				}
			} catch (...) {
			}
		}
	}

	void Install(EventBus& a_events)
	{
		if (g_installed)
			return;
		g_events = &a_events;

		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(50157, 51084) };
		if (!SKSE::stl::install_context_hook(target.address(), kPatchSize, &Detour, kPatchSize)) {
			logs::error("devbench: failed to install console command hook");
			return;
		}
		g_installed = true;
		logs::info("devbench: console command hook installed");
	}
}
