#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

		// A plugin that requested the C-ABI interface (one entry per GetInterface call — a
		// plugin that calls it more than once appears more than once, oldest first). NOTE: the
		// requested revision is NOT captured here — GetApiFunction is a pointer the consumer
		// calls itself, later, on its own; devbench never observes that call or its argument,
		// only that the pointer was handed out. Reporting a revision here would be a guess
		// dressed up as an observation.
		struct Consumer
		{
			std::string   name;  // a_message->sender, or "<?>" if unset
			long long     atEpoch;
			std::uint32_t atFrame;
		};

		// A successful RegisterTool/RegisterToolExtension call over the C-ABI.
		struct Registration
		{
			std::string   kind;  // "tool" | "extension"
			std::string   name;  // tool name, or "<baseTool>:<key>"
			long long     atEpoch;
			std::uint32_t atFrame;
			bool          replaced;  // true if this call overwrote an earlier registration
		};

		// Every GetInterface request seen so far, oldest first. Thread-safe.
		std::vector<Consumer> Consumers();

		// Every successful tool/extension registration seen so far, oldest first. Thread-safe.
		// There is no reliable per-registration caller identity (the C-ABI interface is one
		// shared singleton — see ROADMAP.md's "Event source tagging" item), so this cannot be
		// joined against Consumers() by plugin name; both lists are exposed side by side instead.
		std::vector<Registration> Registrations();
	}
}
