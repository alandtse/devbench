#pragma once

namespace dvb
{
	class ToolRegistry;
	class EventBus;

	/// Register devbench's built-in tools (console, inspect, game, menu, scenario, …)
	/// into the registry. Call once, before Server::Start(), so they appear on both
	/// transports. `a_events` is captured by the scenario tool for event-driven waits.
	void RegisterCoreTools(ToolRegistry& a_registry, EventBus& a_events);
}
