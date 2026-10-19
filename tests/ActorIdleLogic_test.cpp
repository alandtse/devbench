#include "test_framework.h"

#include "ActorIdleLogic.h"

using namespace dvb;
using namespace dvb::ActorIdleLogic;

TEST_CASE("the repeat interval defaults when absent and accepts an integer within the bounds")
{
	CHECK(ParseRepeatMs(json(nullptr)) == kDefaultRepeatMs);
	CHECK(ParseRepeatMs(json(250)) == 250);
	CHECK(ParseRepeatMs(json(60000)) == 60000);
}

TEST_CASE("a repeat interval outside the bounds, or not an integer, is refused")
{
	CHECK(!ParseRepeatMs(json(249)).has_value());
	CHECK(!ParseRepeatMs(json(60001)).has_value());
	CHECK(!ParseRepeatMs(json(1.5)).has_value());
	CHECK(!ParseRepeatMs(json("fast")).has_value());
}

TEST_CASE("a looping idle is replayed once the repeat interval has passed")
{
	CHECK(ReplayDue(2000, 1000, 1000));
	CHECK(ReplayDue(9000, 1000, 1000));
	CHECK(!ReplayDue(1999, 1000, 1000));
}
