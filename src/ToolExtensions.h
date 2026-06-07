#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ToolRegistry.h"  // ToolHandler, json

namespace dvb::ToolExtensions
{
	/// A consumer-registered handler that extends an existing base tool, keyed by a string. Lets a
	/// mod add a sub-capability under a built-in tool (e.g. a custom `inspect` kind or `menu`
	/// interaction) WITHOUT adding a top-level tool, so the agent-facing surface stays small. The
	/// base tool routes to it: `menu invoke name=<key>`, `inspect kind=<key>`. `descriptor` is the
	/// consumer's JSON (surfaced by the base tool's discovery path); `handler` is the same
	/// JSON-in/JSON-out contract as a tool handler.
	struct Entry
	{
		json        descriptor;
		ToolHandler handler;
	};

	/// Register (or replace) a `key` extension under `baseTool` (both matched case-insensitively).
	/// Returns false if it replaced an existing entry. Thread-safe.
	bool Register(std::string a_baseTool, std::string a_key, json a_descriptor, ToolHandler a_handler);

	/// The entry registered under (`baseTool`, `key`), or nullopt. Thread-safe.
	std::optional<Entry> Find(std::string_view a_baseTool, std::string_view a_key);

	/// Sorted display keys registered under `baseTool`. Thread-safe.
	std::vector<std::string> Keys(std::string_view a_baseTool);
}
