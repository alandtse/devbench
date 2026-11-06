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
