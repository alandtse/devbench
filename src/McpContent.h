#pragma once

#include "Json.h"

#include <string>

namespace dvb
{
	/// Wrap a tool's structured result into the MCP `content` array.
	///
	/// cpp-mcp assigns a tool handler's return value *verbatim* to the JSON-RPC
	/// result's `content` field (see mcp_server.cpp), and the MCP spec requires
	/// `content` to be an ARRAY of content blocks. Returning a bare object made
	/// every readable devbench call fail client-side validation with
	/// "expected array, received object". We surface any payload as a single text
	/// block holding its JSON (strings pass through unquoted). The REST facade,
	/// which has no such envelope, keeps returning the value unwrapped.
	///
	/// Header-only and game-independent so it can be unit-tested without a running
	/// Skyrim — this is the exact seam that regressed (see tests/McpContent_test.cpp).
	inline json ToContentBlocks(const json& a_value)
	{
		const std::string text = a_value.is_string() ? a_value.get<std::string>() : a_value.dump(2);
		return json::array({ json{ { "type", "text" }, { "text", text } } });
	}
}
