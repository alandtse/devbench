#include "InputHotkeys.h"

#include "Config.h"
#include "Json.h"
#include "ToolRegistry.h"

#include <RE/Skyrim.h>

#include <atomic>
#include <string>
#include <thread>

namespace dvb
{
	namespace
	{
		ToolRegistry* g_registry = nullptr;
		// Atomic: the sink reads on the main/input thread, the FUCK rebind writes from the render
		// thread. replayPath/replayRestore are never rebound in-menu, so they stay plain.
		std::atomic<int>  g_recordKey{ 0 };
		std::atomic<int>  g_replayKey{ 0 };
		std::atomic<bool> g_recordShift{ false };  // hotkey requires Shift held
		std::atomic<bool> g_replayShift{ false };
		std::string       g_replayPath;
		bool              g_replayRestore = true;

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

		void FireRecordHotkey() { FireAsync(json{ { "action", "toggle" } }); }
		void FireReplayHotkey()
		{
			FireAsync(json{ { "action", "replay" }, { "path", g_replayPath }, { "restoreScene", g_replayRestore } });
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
					const int  recKey = g_recordKey.load(), repKey = g_replayKey.load();
					const bool recShift = g_recordShift.load(), repShift = g_replayShift.load();
					if (recKey && code == recKey && (!recShift || shift)) {
						logs::info("devbench: record hotkey fired (key={}, shift={})", code, shift);
						FireRecordHotkey();
					} else if (repKey && code == repKey && (!repShift || shift)) {
						logs::info("devbench: replay hotkey fired (key={}, shift={})", code, shift);
						FireReplayHotkey();
					} else if ((recKey && code == recKey) || (repKey && code == repKey)) {
						logs::debug("devbench: hotkey base key {} down, shift={} (req rec={}/rep={})",
							code, shift, recShift, repShift);
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
		g_recordKey.store(a_config.recordHotkey);
		g_replayKey.store(a_config.replayHotkey);
		g_recordShift.store(a_config.recordHotkeyShift);
		g_replayShift.store(a_config.replayHotkeyShift);
		g_replayPath = a_config.replayPath;
		g_replayRestore = a_config.replayRestoreScene;
		if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
			idm->AddEventSink(&g_inputSink);
			logs::info("devbench: input hotkeys installed (record={} shift={}, replay={} shift={})",
				g_recordKey.load(), g_recordShift.load(), g_replayKey.load(), g_replayShift.load());
		} else {
			logs::warn("devbench: BSInputDeviceManager unavailable — input hotkeys NOT installed");
		}
	}

	void SetRecordHotkey(int a_scancode, bool a_shift)
	{
		g_recordKey.store(a_scancode);
		g_recordShift.store(a_shift);
		SaveHotkeys(g_recordKey.load(), g_recordShift.load(), g_replayKey.load(), g_replayShift.load());
	}

	void SetReplayHotkey(int a_scancode, bool a_shift)
	{
		g_replayKey.store(a_scancode);
		g_replayShift.store(a_shift);
		SaveHotkeys(g_recordKey.load(), g_recordShift.load(), g_replayKey.load(), g_replayShift.load());
	}

	void GetHotkeys(int& a_recordKey, bool& a_recordShift, int& a_replayKey, bool& a_replayShift)
	{
		a_recordKey = g_recordKey.load();
		a_recordShift = g_recordShift.load();
		a_replayKey = g_replayKey.load();
		a_replayShift = g_replayShift.load();
	}
}
