#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ToolRegistry.h"  // ToolHandler, json

namespace dvb::MenuExtensions
{
	/// A consumer-registered handler for a custom menu, dispatched by the base `menu` tool as
	/// action='invoke', name='<menu>'. Lets a mod expose its menu's interaction without adding a
	/// top-level tool, so the agent-facing surface stays the single `menu` tool. `descriptor` is the
	/// consumer's JSON (surfaced via `menu describe name=<menu>`); `handler` is the same
	/// JSON-in/JSON-out contract as a tool handler.
	struct Entry
	{
		json        descriptor;
		ToolHandler handler;
	};

	/// Register (or replace) the handler for `a_menu` (matched case-insensitively). Returns false if
	/// it replaced an existing entry. Thread-safe.
	bool Register(std::string a_menu, json a_descriptor, ToolHandler a_handler);

	/// The entry registered for `a_menu`, or nullopt. Thread-safe.
	std::optional<Entry> Find(std::string_view a_menu);

	/// Sorted display names of all registered menus. Thread-safe.
	std::vector<std::string> Names();
}
