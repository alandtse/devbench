#include "test_framework.h"
#include "MenuInventory.h"

#include <stdexcept>

namespace
{
	void Install(dvb::MenuInventory& a_inventory, const std::vector<std::string>& a_names)
	{
		CHECK(a_inventory.Install(true, [] {}, [&] { return a_names; }) == dvb::MenuInventory::InstallResult::Ready);
	}
}

TEST_CASE("Menu inventory: late installation seeds an already open Main Menu")
{
	dvb::MenuInventory inventory;
	Install(inventory, { "Main Menu" });
	const auto snapshot = inventory.Snapshot();
	CHECK(snapshot.initialized);
	CHECK(snapshot.openMenus == std::vector<std::string>{ "Main Menu" });
	CHECK(!snapshot.KnownEmpty());
	CHECK(snapshot.KnownAbsent("MessageBoxMenu"));
	CHECK(snapshot.Describe(dvb::json::array())["messageBoxOpen"] == false);
}

TEST_CASE("Menu inventory: unavailable initial snapshot never proves menu or modal absence")
{
	dvb::MenuInventory inventory;
	const auto snapshot = inventory.Snapshot();
	CHECK(!snapshot.initialized);
	CHECK(!snapshot.KnownEmpty());
	CHECK(!snapshot.KnownAbsent("MessageBoxMenu"));
	const auto result = snapshot.Describe(dvb::json::array({ "devbench.selftest" }));
	CHECK(result["messageBoxOpen"].is_null());
	CHECK(result["inventoryInitialized"] == false);
	CHECK(result["inventoryEventsSubscribed"] == false);
	CHECK(result["inventorySource"] == "ui-unavailable-no-sink");
	CHECK(result["registered"][0] == "devbench.selftest");
}

TEST_CASE("Menu inventory: events close the initial menu and maintain later modals")
{
	dvb::MenuInventory inventory;
	Install(inventory, { "Main Menu" });
	inventory.OnEvent("Main Menu", false);
	CHECK(inventory.Snapshot().KnownEmpty());
	inventory.OnEvent("MessageBoxMenu", true);
	CHECK(!inventory.Snapshot().KnownAbsent("MessageBoxMenu"));
	CHECK(inventory.Snapshot().Describe(dvb::json::array())["messageBoxOpen"] == true);
	inventory.OnEvent("MessageBoxMenu", false);
	CHECK(inventory.Snapshot().KnownAbsent("MessageBoxMenu"));
}

TEST_CASE("Menu inventory: an event during seed collection cannot be overwritten")
{
	dvb::MenuInventory inventory;
	CHECK(inventory.Install(true, [] {}, [&] {
		inventory.OnEvent("MessageBoxMenu", true);
		return std::vector<std::string>{ "Main Menu" };
	}) == dvb::MenuInventory::InstallResult::SeedRaced);
	const auto snapshot = inventory.Snapshot();
	CHECK(!snapshot.initialized);
	CHECK(snapshot.openMenus == std::vector<std::string>{ "MessageBoxMenu" });
	CHECK(snapshot.Describe(dvb::json::array())["messageBoxOpen"] == true);
	CHECK(!snapshot.KnownAbsent("Main Menu"));
	CHECK(snapshot.eventsSubscribed);
	CHECK(snapshot.Describe(dvb::json::array())["inventorySource"] == "events-only-incomplete");
}

TEST_CASE("Menu inventory: seeded empty UI is distinguishable from unknown")
{
	dvb::MenuInventory inventory;
	Install(inventory, {});
	CHECK(inventory.Snapshot().KnownEmpty());
	CHECK(inventory.Snapshot().KnownAbsent("MessageBoxMenu"));
	const auto result = inventory.Snapshot().Describe(dvb::json::array());
	CHECK(result["inventoryInitialized"] == true);
	CHECK(result["messageBoxOpen"] == false);
}

TEST_CASE("Menu inventory: snapshots own sorted unique names and cannot mutate state")
{
	dvb::MenuInventory inventory;
	Install(inventory, { "Main Menu", "HUD Menu", "Main Menu" });
	auto snapshot = inventory.Snapshot();
	CHECK(snapshot.openMenus == (std::vector<std::string>{ "HUD Menu", "Main Menu" }));
	snapshot.openMenus.clear();
	inventory.OnEvent("Main Menu", true);
	CHECK(inventory.Snapshot().openMenus.size() == 2);
}

TEST_CASE("Menu inventory: production installer retries unavailable UI once without duplicate subscription")
{
	dvb::MenuInventory inventory;
	int subscriptions = 0, collections = 0;
	auto subscribe = [&] { ++subscriptions; };
	auto collect = [&] { ++collections; return std::vector<std::string>{ "Main Menu" }; };
	CHECK(inventory.Install(false, subscribe, collect) == dvb::MenuInventory::InstallResult::UiUnavailable);
	CHECK(subscriptions == 0);
	CHECK(collections == 0);
	CHECK(inventory.Snapshot().Describe(dvb::json::array())["inventorySource"] == "ui-unavailable-no-sink");
	CHECK(!inventory.Snapshot().KnownEmpty());
	CHECK(inventory.Install(true, subscribe, collect) == dvb::MenuInventory::InstallResult::Ready);
	CHECK(inventory.Install(true, subscribe, collect) == dvb::MenuInventory::InstallResult::Ready);
	CHECK(subscriptions == 1);
	CHECK(collections == 1);
	CHECK(inventory.Snapshot().eventsSubscribed);
	CHECK(inventory.Snapshot().Describe(dvb::json::array())["inventorySource"] == "initial-ui-snapshot-and-events");
	inventory.OnEvent("Main Menu", false);
	CHECK(inventory.Snapshot().KnownEmpty());
}

TEST_CASE("Menu inventory: raced seed retries collection but never installs a second sink")
{
	dvb::MenuInventory inventory;
	int subscriptions = 0;
	auto subscribe = [&] { ++subscriptions; };
	CHECK(inventory.Install(true, subscribe, [&] {
		inventory.OnEvent("MessageBoxMenu", true);
		return std::vector<std::string>{};
	}) == dvb::MenuInventory::InstallResult::SeedRaced);
	CHECK(inventory.Snapshot().Source() == std::string_view("events-only-incomplete"));
	CHECK(!inventory.Snapshot().KnownAbsent("Main Menu"));
	CHECK(inventory.Install(true, subscribe, [] { return std::vector<std::string>{ "MessageBoxMenu" }; }) == dvb::MenuInventory::InstallResult::Ready);
	CHECK(subscriptions == 1);
	CHECK(!inventory.Snapshot().KnownAbsent("MessageBoxMenu"));
}

TEST_CASE("Menu inventory: a snapshot alone cannot claim a subscribed complete inventory")
{
	dvb::MenuInventory inventory;
	CHECK(!inventory.Seed(inventory.BeginSeed(), {}));
	CHECK(!inventory.Snapshot().initialized);
	CHECK(!inventory.Snapshot().eventsSubscribed);
	CHECK(!inventory.Snapshot().KnownAbsent("MessageBoxMenu"));
}

TEST_CASE("Menu inventory: production blocking reader retains unresolved positive names for all negative guards")
{
	dvb::MenuInventory inventory;
	Install(inventory, { "MessageBoxMenu" });
	int lookups = 0;
	const auto blocking = dvb::ReadBlockingMenus(inventory.Snapshot(), true, "Console",
		[&](const std::string& a_name) -> std::optional<dvb::MenuBlockingFlags> {
			++lookups;
			CHECK(a_name == "MessageBoxMenu");
			return std::nullopt;
		});
	CHECK(lookups == 1);
	CHECK(blocking == std::vector<std::string>{ "<unresolved-menu:MessageBoxMenu>" });
	// Actual CheckState/noBlockingMenu, replay precheck and scenario assertion all
	// consume this same production reader's empty/nonempty decision (call-site map
	// supplied with the review). This is not an engine/handler execution claim.
	CHECK(!blocking.empty());
	CHECK(blocking.front() != "MessageBoxMenu"); // not eligible for automatic modal closure
}

TEST_CASE("Menu inventory: production blocking reader preserves resolved flag policy and console exemption")
{
	dvb::MenuInventory inventory;
	Install(inventory, { "Always", "Benign", "Console", "Pause", "Modal", "Cursor", "Freeze" });
	const auto blocking = dvb::ReadBlockingMenus(inventory.Snapshot(), true, "Console",
		[](const std::string& a_name) -> std::optional<dvb::MenuBlockingFlags> {
			CHECK(a_name != "Console");
			return dvb::MenuBlockingFlags{ a_name == "Always", a_name == "Pause" || a_name == "Always",
				a_name == "Modal", a_name == "Cursor", a_name == "Freeze" };
		});
	CHECK(blocking == (std::vector<std::string>{ "Cursor", "Freeze", "Modal", "Pause" }));
}

TEST_CASE("Menu inventory: production blocking reader never consults unavailable UI or infers unknown absence")
{
	dvb::MenuInventory inventory;
	int lookups = 0;
	auto lookup = [&](const std::string&) -> std::optional<dvb::MenuBlockingFlags> { ++lookups; return std::nullopt; };
	CHECK(dvb::ReadBlockingMenus(inventory.Snapshot(), true, "Console", lookup) == std::vector<std::string>{ "<menu-inventory-unknown>" });
	Install(inventory, {});
	CHECK(dvb::ReadBlockingMenus(inventory.Snapshot(), false, "Console", lookup) == std::vector<std::string>{ "<ui-unavailable>" });
	CHECK(lookups == 0);
	CHECK(dvb::ReadBlockingMenus(inventory.Snapshot(), true, "Console", lookup).empty());
}

TEST_CASE("Menu inventory: collection failure preserves subscription and unknown absence for later retry")
{
	dvb::MenuInventory inventory;
	int subscriptions = 0;
	bool caught = false;
	try {
		(void)inventory.Install(true, [&] { ++subscriptions; }, []() -> std::vector<std::string> {
			throw std::runtime_error("injected collection failure");
		});
	} catch (const std::runtime_error&) { caught = true; }
	CHECK(caught);
	CHECK(inventory.Snapshot().eventsSubscribed);
	CHECK(!inventory.Snapshot().KnownEmpty());
	CHECK(inventory.Snapshot().Source() == std::string_view("events-only-incomplete"));
	CHECK(inventory.Install(true, [&] { ++subscriptions; }, [] { return std::vector<std::string>{}; }) == dvb::MenuInventory::InstallResult::Ready);
	CHECK(subscriptions == 1);
	CHECK(inventory.Snapshot().KnownEmpty());
}
