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
