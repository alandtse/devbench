// Regression coverage for the MCP `content` envelope. cpp-mcp assigns a tool
// handler's return value verbatim to result["content"], which the MCP spec
// requires to be an ARRAY of content blocks. A bare object here is what made
// every readable devbench call fail client-side with "expected array, received
// object" — these cases pin the array shape so it can't silently regress.

#include "test_framework.h"

#include "McpContent.h"

using dvb::json;
using dvb::ToContentBlocks;

namespace
{
	// A valid content block is an object with a string "type" and matching payload.
	bool IsTextBlock(const json& a_block)
	{
		return a_block.is_object() &&
		       a_block.contains("type") && a_block["type"] == "text" &&
		       a_block.contains("text") && a_block["text"].is_string();
	}
}

TEST_CASE("content is always a non-empty array of blocks")
{
	for (const json& v : { json::object(), json("hi"), json(42), json(nullptr),
			 json::array({ 1, 2 }), json{ { "ok", true } } }) {
		const json content = ToContentBlocks(v);
		CHECK_MESSAGE(content.is_array(), "content must be an array for value: " + v.dump());
		CHECK(!content.empty());
		CHECK(IsTextBlock(content[0]));
	}
}

TEST_CASE("structured object payload is embedded as JSON text")
{
	const json value = { { "feature", "TAA" }, { "enabled", true }, { "weight", 0.5 } };
	const json content = ToContentBlocks(value);

	CHECK(content.is_array());
	CHECK(content.size() == 1);
	CHECK(IsTextBlock(content[0]));

	// The text block round-trips back to the original structured value.
	const json parsed = json::parse(content[0]["text"].get<std::string>());
	CHECK(parsed == value);
}

TEST_CASE("string payload passes through unquoted")
{
	const json content = ToContentBlocks(json("pong"));

	CHECK(content.is_array());
	CHECK(IsTextBlock(content[0]));
	// A string is surfaced as-is, not JSON-re-encoded to "\"pong\"".
	CHECK(content[0]["text"] == "pong");
}
