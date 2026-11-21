#include "test_framework.h"

#include "VRControllerProps.h"

using namespace dvb::VRControllerProps;

TEST_CASE("the synthetic supported-buttons mask covers both grip ids the engine accepts")
{
	CHECK((kSyntheticSupportedButtons & Bit(2)) != 0);
	CHECK((kSyntheticSupportedButtons & Bit(34)) != 0);
	CHECK((kSyntheticSupportedButtons & Bit(33)) != 0);
}

TEST_CASE("synthetic axes follow the Oculus Touch layout")
{
	CHECK(SyntheticAxisType(0) == kAxisJoystick);
	CHECK(SyntheticAxisType(1) == kAxisTrigger);
	CHECK(SyntheticAxisType(2) == kAxisTrigger);
	CHECK(SyntheticAxisType(3) == kAxisNone);
	CHECK(SyntheticAxisType(4) == kAxisNone);
}

TEST_CASE("a controller index stays recent only for the release grace window")
{
	RecentIndices recent;
	CHECK(!recent.Recent(2, 1000));
	recent.Note(2, 1000);
	CHECK(recent.Recent(2, 1000 + RecentIndices::kGraceMs));
	CHECK(!recent.Recent(2, 1001 + RecentIndices::kGraceMs));
	CHECK(!recent.Recent(3, 1000));
}

TEST_CASE("two controller indices are tracked and the oldest is evicted by a third")
{
	RecentIndices recent;
	recent.Note(2, 100);
	recent.Note(3, 200);
	CHECK(recent.Recent(2, 150));
	CHECK(recent.Recent(3, 250));
	recent.Note(2, 300);
	recent.Note(4, 310);
	CHECK(recent.Recent(2, 320));
	CHECK(!recent.Recent(3, 320));
	CHECK(recent.Recent(4, 320));
}
