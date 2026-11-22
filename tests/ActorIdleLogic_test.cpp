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

TEST_CASE("the locomotion speed defaults when absent and accepts a number within the bounds")
{
	CHECK(ParseLocomotionSpeed(json(nullptr), false) == static_cast<float>(kDefaultLocomotionSpeed));
	CHECK(ParseLocomotionSpeed(json(0), false) == 0.0f);
	CHECK(ParseLocomotionSpeed(json(250.5), false) == 250.5f);
	CHECK(ParseLocomotionSpeed(json(1000), false) == 1000.0f);
}

TEST_CASE("a sprint defaults to a faster speed and still honours an explicit one")
{
	CHECK(ParseLocomotionSpeed(json(nullptr), true) == static_cast<float>(kDefaultSprintSpeed));
	CHECK(ParseLocomotionSpeed(json(450), true) == 450.0f);
	CHECK(!ParseLocomotionSpeed(json(1001), true).has_value());
}

TEST_CASE("a locomotion speed outside the bounds, or not a number, is refused")
{
	CHECK(!ParseLocomotionSpeed(json(-1), false).has_value());
	CHECK(!ParseLocomotionSpeed(json(1000.5), false).has_value());
	CHECK(!ParseLocomotionSpeed(json("fast"), false).has_value());
	CHECK(!ParseLocomotionSpeed(json(true), false).has_value());
}
