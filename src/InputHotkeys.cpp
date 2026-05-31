#include "InputHotkeys.h"

#include "Config.h"
#include "Json.h"
#include "ToolRegistry.h"

#include <RE/Skyrim.h>

#include <string>
#include <thread>

namespace dvb
{
	namespace
	{
		ToolRegistry* g_registry = nullptr;
		int           g_recordKey = 0;
		int           g_replayKey = 0;
		bool          g_recordShift = false;  // hotkey requires Shift held
		bool          g_replayShift = false;
		std::string   g_replayPath;
		bool          g_replayRestore = true;

		// Current Shift state via the OS (matches CS's InputCombo::MatchesKeyboardCombo) —
		// robust regardless of whether/when Shift events arrive on the input sink, which is
		// why this replaced the earlier event-stream tracking.
		bool ShiftHeld()
		{
			return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		}

		// The sink fires on the main thread, but the record tool marshals back to the main
		// thread via RunAndWait — invoking it inline would deadlock. So dispatch on a detached
		// thread; the handler runs there and marshals as usual.
		void FireAsync(json a_args)
		{
			if (!g_registry)
				return;
			std::thread([args = std::move(a_args)]() {
				try {
					g_registry->Invoke("record", args, ToolContext{ "hotkey" });
				} catch (...) {
				}
			}).detach();
		}

		bool ConsoleOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::Console::MENU_NAME);
		}

		class InputSink : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events,
				RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!a_events)
					return RE::BSEventNotifyControl::kContinue;
				for (auto* e = *a_events; e; e = e->next) {
					auto* btn = e->AsButtonEvent();
					if (!btn || !btn->IsDown())  // first frame down only — no key-repeat
						continue;
					if (btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard)
						continue;
					if (ConsoleOpen())  // don't fire on keystrokes typed into the console
						continue;
					const int  code = static_cast<int>(btn->GetIDCode());
					const bool shift = ShiftHeld();
					if (g_recordKey && code == g_recordKey && (!g_recordShift || shift)) {
						logs::info("devbench: record hotkey fired (key={}, shift={})", code, shift);
						FireAsync(json{ { "action", "toggle" } });
					} else if (g_replayKey && code == g_replayKey && (!g_replayShift || shift)) {
						logs::info("devbench: replay hotkey fired (key={}, shift={})", code, shift);
						FireAsync(json{ { "action", "replay" }, { "path", g_replayPath }, { "restoreScene", g_replayRestore } });
					} else if ((g_recordKey && code == g_recordKey) || (g_replayKey && code == g_replayKey)) {
						logs::debug("devbench: hotkey base key {} down, shift={} (req rec={}/rep={})",
							code, shift, g_recordShift, g_replayShift);
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		InputSink g_inputSink;
	}

	void InstallInputHotkeys(ToolRegistry& a_registry, const Config& a_config)
	{
		if (a_config.recordHotkey == 0 && a_config.replayHotkey == 0)
			return;  // opt-in; nothing configured
		g_registry = &a_registry;
		g_recordKey = a_config.recordHotkey;
		g_replayKey = a_config.replayHotkey;
		g_recordShift = a_config.recordHotkeyShift;
		g_replayShift = a_config.replayHotkeyShift;
		g_replayPath = a_config.replayPath;
		g_replayRestore = a_config.replayRestoreScene;
		if (auto* idm = RE::BSInputDeviceManager::GetSingleton())
			idm->AddEventSink(&g_inputSink);
		logs::info("devbench: input hotkeys installed (record={} shift={}, replay={} shift={})",
			g_recordKey, g_recordShift, g_replayKey, g_replayShift);
	}
}
