#include "GameEvents.h"

#include "EventBus.h"
#include "Json.h"

#include <mutex>
#include <unordered_set>

namespace dvb
{
	namespace
	{
		EventBus* g_bus = nullptr;

		std::mutex g_menuMutex;
		std::unordered_set<std::string> g_openMenus;  // live set, updated from the sink

		// Sink for menu open/close — covers loading screens, the main menu, and the
		// new-game message boxes / RaceSex menu that block automated flows.
		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event) {
					const std::string name = a_event->menuName.c_str();
					{
						std::lock_guard lock(g_menuMutex);
						if (a_event->opening)
							g_openMenus.insert(name);
						else
							g_openMenus.erase(name);
					}
					if (g_bus)
						g_bus->Publish("menu", json{ { "name", name }, { "opening", a_event->opening } });
				}
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

	std::vector<std::string> GetOpenMenus()
	{
		std::lock_guard<std::mutex> lock(g_menuMutex);
		return { g_openMenus.begin(), g_openMenus.end() };
	}
}
