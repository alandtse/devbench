// Host-independent coverage for ToolRegistry: registration, dispatch, and the
// exception->ToolResult folding that the adapters rely on (no exception is ever
// allowed to cross the adapter boundary).

#include "test_framework.h"

#include "ToolRegistry.h"

using dvb::json;
using dvb::ToolContext;
using dvb::ToolDescriptor;
using dvb::ToolError;
using dvb::ToolRegistry;
using dvb::ToolResult;

namespace
{
	ToolDescriptor Desc(std::string a_name)
	{
		ToolDescriptor d;
		d.name = std::move(a_name);
		d.description = "test tool";
		return d;
	}
}

TEST_CASE("a schema-less tool still gets a valid JSON Schema object")
{
	// {} fails MCP's required root "type":"object" -- ToolDescriptor's default and
	// DefaultInputSchema() (also used by the C-ABI RegisterTool for an omitted
	// inputSchema) must agree, or a schema-less tool from either path breaks a
	// strict MCP client.
	CHECK(ToolDescriptor{}.inputSchema == dvb::DefaultInputSchema());
	CHECK(dvb::DefaultInputSchema().value("type", std::string{}) == "object");
}

TEST_CASE("register then invoke returns the handler payload")
{
	ToolRegistry reg;
	const bool   fresh = reg.Register(Desc("echo"), [](const json& a_args, const ToolContext&) {
		return a_args;
	});

	CHECK(fresh);  // first registration of this name
	CHECK(reg.Has("echo"));

	const ToolResult r = reg.Invoke("echo", json{ { "v", 7 } }, ToolContext{});
	CHECK(r.ok);
	CHECK(r.value == (json{ { "v", 7 } }));
	CHECK(r.errorCode == 0);
}

TEST_CASE("unknown tool yields a 404 result, never throws")
{
	ToolRegistry reg;
	ToolResult   r;
	CHECK_NOTHROW(r = reg.Invoke("nope", json::object(), ToolContext{}));
	CHECK(!r.ok);
	CHECK(r.errorCode == 404);
}

TEST_CASE("ToolError is folded into a failure result with its code")
{
	ToolRegistry reg;
	reg.Register(Desc("bad"), [](const json&, const ToolContext&) -> json {
		throw ToolError{ 422, "unprocessable" };
	});

	const ToolResult r = reg.Invoke("bad", json::object(), ToolContext{});
	CHECK(!r.ok);
	CHECK(r.errorCode == 422);
	CHECK(r.errorMessage == "unprocessable");
}

TEST_CASE("a non-ToolError exception folds to a 500 result")
{
	ToolRegistry reg;
	reg.Register(Desc("boom"), [](const json&, const ToolContext&) -> json {
		throw std::runtime_error("kaboom");
	});

	const ToolResult r = reg.Invoke("boom", json::object(), ToolContext{});
	CHECK(!r.ok);
	CHECK(r.errorCode == 500);
}

TEST_CASE("re-registering the same name reports replacement")
{
	ToolRegistry reg;
	CHECK(reg.Register(Desc("dup"), [](const json&, const ToolContext&) { return json::object(); }));
	// Second Register of an existing name returns false (replaced).
	CHECK(!reg.Register(Desc("dup"), [](const json&, const ToolContext&) { return json::object(); }));
}

TEST_CASE("Describe and List reflect registered tools")
{
	ToolRegistry reg;
	reg.Register(Desc("a"), [](const json&, const ToolContext&) { return json::object(); });
	reg.Register(Desc("b"), [](const json&, const ToolContext&) { return json::object(); });

	const auto a = reg.Describe("a");
	CHECK(a.has_value());
	CHECK(a->description == "test tool");
	CHECK(!reg.Describe("missing").has_value());
	CHECK(reg.List().size() == 2);
}

TEST_CASE("registration listener fires for tools added after wiring")
{
	ToolRegistry reg;
	int          seen = 0;
	std::string  lastName;
	reg.SetRegistrationListener([&](const ToolDescriptor& d) {
		++seen;
		lastName = d.name;
	});

	reg.Register(Desc("late"), [](const json&, const ToolContext&) { return json::object(); });
	CHECK(seen == 1);
	CHECK(lastName == "late");
}

namespace
{
	ToolDescriptor Declared(std::string a_name, json a_schema)
	{
		ToolDescriptor d = Desc(std::move(a_name));
		d.inputSchema = std::move(a_schema);
		return d;
	}

	const json kSchema = json{ { "type", "object" }, { "properties", { { "action", json::object() }, { "args", json::object() } } } };

	auto EmptyHandler()
	{
		return [](const json&, const ToolContext&) { return json::object(); };
	}
}

TEST_CASE("an unknown top-level key is reported in the reply's warnings")
{
	ToolRegistry reg;
	reg.Register(Declared("t", kSchema), EmptyHandler());

	const ToolResult r = reg.Invoke("t", json{ { "action", "go" }, { "params", json::object() } }, ToolContext{});
	CHECK(r.ok);
	CHECK(r.value.contains("warnings"));
	CHECK(r.value["warnings"].size() == 1);
	const std::string w = r.value["warnings"][0];
	CHECK(w.find("params") != std::string::npos);
	CHECK(w.find("action, args") != std::string::npos);  // names the accepted keys
}

TEST_CASE("declared keys produce no warning")
{
	ToolRegistry reg;
	reg.Register(Declared("t", kSchema), EmptyHandler());

	const ToolResult r = reg.Invoke("t", json{ { "action", "go" }, { "args", json::object() } }, ToolContext{});
	CHECK(r.ok);
	CHECK(!r.value.contains("warnings"));
}

TEST_CASE("schemas that do not enumerate their keys are never checked")
{
	ToolRegistry reg;
	reg.Register(Desc("schemaless"), EmptyHandler());
	reg.Register(Declared("open", json{ { "type", "object" }, { "properties", { { "a", json::object() } } }, { "additionalProperties", true } }),
		EmptyHandler());
	reg.Register(Declared("composed", json{ { "type", "object" }, { "properties", { { "a", json::object() } } }, { "oneOf", json::array() } }),
		EmptyHandler());

	for (const char* name : { "schemaless", "open", "composed" }) {
		const ToolResult r = reg.Invoke(name, json{ { "anything", 1 } }, ToolContext{});
		CHECK(r.ok);
		CHECK(!r.value.contains("warnings"));
	}
}

TEST_CASE("a warning is appended to a handler's own warnings array")
{
	ToolRegistry reg;
	reg.Register(Declared("t", kSchema), [](const json&, const ToolContext&) { return json{ { "warnings", json::array({ "own" }) } }; });

	const ToolResult r = reg.Invoke("t", json{ { "extra", 1 } }, ToolContext{});
	CHECK(r.value["warnings"].size() == 2);
	CHECK(r.value["warnings"][0] == "own");
}

TEST_CASE("strict mode rejects an unknown key with 400 and does not run the handler")
{
	ToolRegistry reg;
	bool         ran = false;
	reg.Register(Declared("t", kSchema), [&](const json&, const ToolContext&) {
		ran = true;
		return json::object();
	});
	reg.SetStrictArgs(true);

	const ToolResult r = reg.Invoke("t", json{ { "params", 1 } }, ToolContext{});
	CHECK(!r.ok);
	CHECK(r.errorCode == 400);
	CHECK(!ran);

	ToolContext internal;
	internal.internal = true;  // scenario/replay steps are never rejected
	CHECK(reg.Invoke("t", json{ { "params", 1 } }, internal).ok);
}
