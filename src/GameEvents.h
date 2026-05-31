#pragma once

#include <cstdint>

namespace dvb
{
	class EventBus;

	/// Install game event sources that publish into `a_bus`:
	///  - a MenuOpenCloseEvent sink → "menu" events { name, opening } (loading screens,
	///    main menu, new-game message boxes, etc.).
	/// SKSE-messaging lifecycle events are forwarded separately via OnSKSEMessage.
	/// Call once, after the server's EventBus exists (kDataLoaded).
	void InstallGameEvents(EventBus& a_bus);

	/// Forward an SKSE MessagingInterface message type; publishes a "lifecycle" event
	/// (dataLoaded / newGame / preLoadGame / postLoadGame / saveGame / deleteGame).
	/// No-op until InstallGameEvents has run.
	void OnSKSEMessage(std::uint32_t a_type);
}
