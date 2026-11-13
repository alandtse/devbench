#pragma once

#include "Json.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace dvb
{
	struct MenuInventorySnapshot
	{
		std::vector<std::string> openMenus;
		bool initialized{ false };
		bool eventsSubscribed{ false };

		const char* Source() const
		{
			return initialized ? "initial-ui-snapshot-and-events" :
			       eventsSubscribed ? "events-only-incomplete" : "ui-unavailable-no-sink";
		}

		bool KnownAbsent(std::string_view a_name) const
		{
			return initialized && std::find(openMenus.begin(), openMenus.end(), a_name) == openMenus.end();
		}

		bool KnownEmpty() const { return initialized && openMenus.empty(); }

		json Describe(const json& a_registered) const
		{
			const bool modal = std::find(openMenus.begin(), openMenus.end(), "MessageBoxMenu") != openMenus.end();
			return json{
				{ "openMenus", openMenus },
				{ "inventoryInitialized", initialized },
				{ "inventoryEventsSubscribed", eventsSubscribed },
				{ "inventorySource", Source() },
				// A positive observation remains useful; absence is unknown until seeded.
				{ "messageBoxOpen", (modal || initialized) ? json(modal) : json(nullptr) },
				{ "registered", a_registered },
			};
		}
	};

	struct MenuBlockingFlags
	{
		bool alwaysOpen;
		bool pausesGame;
		bool modal;
		bool usesCursor;
		bool freezeFramePause;
	};

	// Shared production reader used by noBlockingMenu, replay prechecks and scenario
	// assertions. A positive inventory name with no live object is unresolved, not
	// benign. Only the main-thread caller may supply the engine lookup.
	template <class Lookup>
	std::vector<std::string> ReadBlockingMenus(const MenuInventorySnapshot& a_inventory,
		bool a_uiAvailable, std::string_view a_consoleName, Lookup&& a_lookup)
	{
		if (!a_inventory.initialized)
			return { "<menu-inventory-unknown>" };
		if (!a_uiAvailable)
			return { "<ui-unavailable>" };
		std::vector<std::string> out;
		for (const auto& name : a_inventory.openMenus) {
			if (name == a_consoleName)
				continue;
			const std::optional<MenuBlockingFlags> flags = a_lookup(name);
			if (!flags)
				out.push_back("<unresolved-menu:" + name + ">");
			else if (!flags->alwaysOpen && (flags->pausesGame || flags->modal || flags->usesCursor || flags->freezeFramePause))
				out.push_back(name);
		}
		return out;
	}

	// Production event inventory. UI enumeration is supplied only by the owning
	// main-thread install, never performed by listener/watchdog readers.
	class MenuInventory
	{
	public:
		enum class InstallResult { UiUnavailable, Ready, SeedRaced };

		// Main-thread-only installer, also used by the existing deferred milestone
		// drain. Engine callbacks run WITHOUT mutex_: subscribing may deliver events.
		// Readers/event callbacks remain synchronized by mutex_. Repeated installs
		// neither add another sink nor reseed a complete inventory.
		template <class Subscribe, class Collect>
		InstallResult Install(bool a_uiAvailable, Subscribe&& a_subscribe, Collect&& a_collect)
		{
			if (!a_uiAvailable)
				return InstallResult::UiUnavailable;
			if (!Snapshot().eventsSubscribed) {
				a_subscribe();
				std::lock_guard lock(mutex_);
				eventsSubscribed_ = true;
			}
			if (Snapshot().initialized)
				return InstallResult::Ready;
			const auto revision = BeginSeed();
			return Seed(revision, a_collect()) ? InstallResult::Ready : InstallResult::SeedRaced;
		}

		std::uint64_t BeginSeed() const
		{
			std::lock_guard lock(mutex_);
			return revision_;
		}

		bool Seed(std::uint64_t a_revision, const std::vector<std::string>& a_names)
		{
			std::unordered_set<std::string> names(a_names.begin(), a_names.end());
			std::lock_guard lock(mutex_);
			// Never overwrite an event delivered during collection with a stale seed.
			if (!eventsSubscribed_ || revision_ != a_revision)
				return false;
			openMenus_.swap(names);
			initialized_ = true;
			++revision_;
			return true;
		}

		void OnEvent(const std::string& a_name, bool a_opening)
		{
			std::lock_guard lock(mutex_);
			if (a_opening)
				openMenus_.insert(a_name);
			else
				openMenus_.erase(a_name);
			++revision_;
		}

		MenuInventorySnapshot Snapshot() const
		{
			std::lock_guard lock(mutex_);
			MenuInventorySnapshot out{ { openMenus_.begin(), openMenus_.end() }, initialized_, eventsSubscribed_ };
			std::sort(out.openMenus.begin(), out.openMenus.end());
			return out;
		}

	private:
		mutable std::mutex mutex_;
		std::unordered_set<std::string> openMenus_;
		std::uint64_t revision_{ 0 };
		bool initialized_{ false };
		bool eventsSubscribed_{ false };
	};
}
