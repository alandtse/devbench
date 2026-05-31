#include "GameEvents.h"

#include "EventBus.h"
#include "Json.h"

namespace dvb
{
	namespace
	{
		EventBus* g_bus = nullptr;

		// Sink for menu open/close — covers loading screens, the main menu, and the
		// new-game message boxes / RaceSex menu that block automated flows.
		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && g_bus)
					g_bus->Publish("menu", json{ { "name", a_event->menuName.c_str() }, { "opening", a_event->opening } });
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		MenuSink g_menuSink;
	}

	void InstallGameEvents(EventBus& a_bus)
	{
		g_bus = &a_bus;
		if (auto* ui = RE::UI::GetSingleton())
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink);
	}

	void OnSKSEMessage(std::uint32_t a_type)
	{
		if (!g_bus)
			return;
		const char* event = nullptr;
		switch (a_type) {
		case SKSE::MessagingInterface::kDataLoaded:
			event = "dataLoaded";
			break;
		case SKSE::MessagingInterface::kNewGame:
			event = "newGame";
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			event = "preLoadGame";
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			event = "postLoadGame";
			break;
		case SKSE::MessagingInterface::kSaveGame:
			event = "saveGame";
			break;
		case SKSE::MessagingInterface::kDeleteGame:
			event = "deleteGame";
			break;
		default:
			return;
		}
		g_bus->Publish("lifecycle", json{ { "event", event } });
	}
}
