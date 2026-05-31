#pragma once

namespace dvb
{
	class ToolRegistry;

	/// Register devbench's built-in tools (console, inspect, …) into the registry.
	/// Call once, before Server::Start(), so they appear on both transports.
	void RegisterCoreTools(ToolRegistry& a_registry);
}
