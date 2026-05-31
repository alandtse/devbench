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
		bool          g_shiftDown = false;  // tracked from the input stream

		// DXScanCodes for the modifier keys we track.
		constexpr int kLShift = 42;  // 0x2A
		constexpr int kRShift = 54;  // 0x36

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
					if (!btn || btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard)
						continue;
					const int code = static_cast<int>(btn->GetIDCode());

					// Track Shift held-state across batches (you hold Shift, then tap the key in a
					// later batch). IsPressed() is true while down/held, false on the up event.
					if (code == kLShift || code == kRShift) {
						g_shiftDown = btn->IsPressed();
						continue;
					}

					if (!btn->IsDown())  // hotkeys: first frame down only — no key-repeat
						continue;
					if (ConsoleOpen())  // don't fire on keystrokes typed into the console
						continue;
					if (g_recordKey && code == g_recordKey && (!g_recordShift || g_shiftDown))
						FireAsync(json{ { "action", "toggle" } });
					else if (g_replayKey && code == g_replayKey && (!g_replayShift || g_shiftDown))
						FireAsync(json{ { "action", "replay" }, { "path", g_replayPath }, { "restoreScene", g_replayRestore } });
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
