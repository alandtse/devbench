#include "test_framework.h"

#include "GfxLogic.h"

using namespace dvb::GfxLogic;

TEST_CASE("a gfx path splits on dots and drops empty segments")
{
	using Segments = std::vector<std::string>;
	CHECK((SplitPath("_root.a.0.b") == Segments{ "_root", "a", "0", "b" }));
	CHECK((SplitPath("_root") == Segments{ "_root" }));
	CHECK(SplitPath("").empty());
	CHECK((SplitPath(".a..b.") == Segments{ "a", "b" }));
}

TEST_CASE("only short all-digit gfx path segments are array indexes")
{
	CHECK(IsIndex("0"));
	CHECK(IsIndex("123"));
	CHECK(IsIndex("999999999"));
	CHECK(!IsIndex(""));
	CHECK(!IsIndex("a1"));
	CHECK(!IsIndex("-1"));
	CHECK(!IsIndex("1234567890"));
}

TEST_CASE("a gfx read stops spending once its node cap is reached")
{
	Budget budget;
	CHECK(!budget.Exhausted(2));
	budget.nodes = 1;
	CHECK(!budget.Exhausted(2));
	CHECK(!budget.truncated);
	budget.nodes = 2;
	CHECK(budget.Exhausted(2));
	CHECK(budget.truncated);
}

TEST_CASE("a gfx limit is absent, or an integer that fits uint32")
{
	using dvb::json;
	CHECK(ReadLimit(json::object(), 500u) == 500u);
	CHECK(ReadLimit(json{ { "limit", 0 } }, 500) == 0u);
	CHECK(ReadLimit(json{ { "limit", 4294967295ULL } }, 500) == 4294967295u);
	CHECK(!ReadLimit(json{ { "limit", -1 } }, 500).has_value());
	CHECK(!ReadLimit(json{ { "limit", 4294967296ULL } }, 500).has_value());
	CHECK(!ReadLimit(json{ { "limit", 1.5 } }, 500).has_value());
	CHECK(!ReadLimit(json{ { "limit", "5" } }, 500).has_value());
}

TEST_CASE("valid UTF-8 menu text passes through and Windows-1252 text is transcoded")
{
	CHECK(ToUtf8("") == "");
	CHECK(ToUtf8("plain") == "plain");
	CHECK(ToUtf8("caf\xC3\xA9") == "caf\xC3\xA9");
	CHECK(ToUtf8("caf\xE9") == "caf\xC3\xA9");
	CHECK(ToUtf8("\x93hi\x94") == "\xE2\x80\x9Chi\xE2\x80\x9D");
	CHECK(ToUtf8("\x80") == "\xE2\x82\xAC");
	CHECK(ToUtf8("\xC3") == "\xC3\x83");
	CHECK(ToUtf8("\xC0\x80") == "\xC3\x80\xE2\x82\xAC");
}
