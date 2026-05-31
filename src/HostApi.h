#pragma once

namespace dvb
{
	class ToolRegistry;
	class EventBus;

	// Provider side of the cross-plugin C-ABI (DevBenchAPI). Lets other SKSE plugins
	// register tools / emit events into this host's registry and event bus.
	namespace HostApi
	{
		// Wire the host interface to the registry + bus. Call once at kDataLoaded,
		// before consumers request the interface (they do so at kPostLoad).
		void Init(ToolRegistry& a_registry, EventBus& a_events);

		// Handle a DevBenchMessage::kMessage_GetInterface request. Call from the SKSE
		// message listener for every message — it no-ops unless it's the request.
		void OnInterfaceRequest(SKSE::MessagingInterface::Message* a_message);
	}
}
