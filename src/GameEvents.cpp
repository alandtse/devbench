#include "GameEvents.h"

#include "EventBus.h"
#include "Json.h"
#include "Recording.h"

#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <unordered_set>

namespace dvb
{
	namespace
	{
		EventBus* g_bus = nullptr;

		std::mutex                      g_menuMutex;
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

		// Player's parent-cell formID, to tell a real cell change from a neighbor preloading.
		std::atomic<std::uint32_t> g_lastPlayerCell{ 0 };
		// Previous cell's interior flag and worldspace — needed to classify the transition type.
		std::atomic<bool>          g_lastCellInterior{ false };
		std::atomic<std::uint32_t> g_lastPlayerWorldspace{ 0 };

		// Sink for cell loads — the authoritative "player entered cell X" signal, whether they
		// walked through a door, typed coc, or fast-travelled. Publishes scene.cellLoaded and
		// feeds Recording so a transition becomes a reproducible `coc <cell>` step.
		class CellSink : public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellFullyLoadedEvent*,
				RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override
			{
				// Use the player's actual parent cell — the event also fires for preloaded
				// neighbors, which aren't transitions.
				auto* pc = RE::PlayerCharacter::GetSingleton();
				auto* cell = pc ? pc->GetParentCell() : nullptr;
				if (!cell)
					return RE::BSEventNotifyControl::kContinue;
				const std::uint32_t id = cell->GetFormID();
				if (g_lastPlayerCell.exchange(id) == id)
					return RE::BSEventNotifyControl::kContinue;  // player's cell didn't change

				const bool        interior = cell->IsInteriorCell();
				const char*       eid = cell->GetFormEditorID();
				const std::string editorId = (eid && *eid) ? eid : std::string{};
				if (g_bus)
					g_bus->Publish("scene.cellLoaded", json{ { "cell", editorId }, { "formID", id }, { "interior", interior } });

				// Interiors: coc (unique editor ids). Exteriors: cow <ws> <gx> <gy> — exterior
				// editor ids repeat across worldspaces so coc is ambiguous (e.g. Soul Cairn).
				// Same-worldspace exterior→exterior: no cow; setpos handles the crossing.
				auto*               ws = interior ? nullptr : pc->GetWorldspace();
				const std::uint32_t wsId = ws ? ws->GetFormID() : 0;
				const bool          prevInterior = g_lastCellInterior.exchange(interior);
				const std::uint32_t prevWsId = g_lastPlayerWorldspace.exchange(wsId);

				std::string cmd;
				if (interior) {
					if (!editorId.empty())
						cmd = "coc " + editorId;
				} else if (!prevInterior && wsId != 0 && wsId == prevWsId) {
					// same-worldspace exterior→exterior: streaming handles it, no cow
				} else if (ws) {
					if (const char* wsEid = ws->GetFormEditorID(); wsEid && *wsEid) {
						const auto pos = pc->GetPosition();
						cmd = std::format("cow {} {} {}", wsEid,
							static_cast<int>(std::floor(pos.x / 4096.0f)),
							static_cast<int>(std::floor(pos.y / 4096.0f)));
					}
				}
				Recording::NoteCellChange(cmd);  // no-op if not recording / cmd empty
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		CellSink g_cellSink;

		// NOTE: do NOT sink TESActivateEvent here. It is one of the engine's chattiest events
		// (fires for every activation by every actor), and a per-event sink on the main thread
		// starves the SKSE task queue — observed live as devbench's main-thread reads (record
		// start, inspect) timing out in a populated cell. A "scene.activate" / faithful door
		// replay, if ever wanted, needs a lower-frequency or off-main-thread approach, not this.
	}

	void InstallGameEvents(EventBus& a_bus)
	{
		g_bus = &a_bus;
		if (auto* ui = RE::UI::GetSingleton())
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink);
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton())
			holder->AddEventSink<RE::TESCellFullyLoadedEvent>(&g_cellSink);
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
