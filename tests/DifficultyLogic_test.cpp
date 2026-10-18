#include "test_framework.h"

#include "DifficultyLogic.h"

using namespace dvb;
using namespace dvb::DifficultyLogic;

TEST_CASE("difficulty level names run from Novice to Legendary")
{
	CHECK(LevelName(0) == "Novice");
	CHECK(LevelName(3) == "Expert");
	CHECK(LevelName(5) == "Legendary");
	CHECK(LevelName(-1).empty());
	CHECK(LevelName(6).empty());
}

TEST_CASE("a difficulty parses from an integer 0..5 or a level name in any case")
{
	CHECK(ParseLevel(json(0)) == 0);
	CHECK(ParseLevel(json(5)) == 5);
	CHECK(ParseLevel(json("adept")) == 2);
	CHECK(ParseLevel(json("LEGENDARY")) == 5);
}

TEST_CASE("a difficulty outside the levels, or of another type, is refused")
{
	CHECK(!ParseLevel(json(-1)).has_value());
	CHECK(!ParseLevel(json(6)).has_value());
	CHECK(!ParseLevel(json("survival")).has_value());
	CHECK(!ParseLevel(json(2.5)).has_value());
	CHECK(!ParseLevel(json(true)).has_value());
	CHECK(!ParseLevel(json()).has_value());
}
