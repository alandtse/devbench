#include "test_framework.h"

#include "HeldLightsLogic.h"

using namespace dvb::HeldLights;

namespace
{
	std::vector<Search> BothViews(bool a_thirdFound = true, bool a_firstFound = true, bool a_firstLoaded = true)
	{
		return { Search{ "thirdPerson", "WEAPON", true, a_thirdFound }, Search{ "firstPerson", "WEAPON", a_firstLoaded, a_firstFound } };
	}
}

TEST_CASE("held lights count entries and unique lights separately")
{
	const auto s = Summarize(BothViews(), { { 1, Membership::kActive }, { 1, Membership::kNone }, { 2, Membership::kNone },
											  { 3, Membership::kShadow }, { 3, Membership::kShadow } });
	CHECK(s.entries == 5);
	CHECK(s.entriesInScene == 3);
	CHECK(s.uniqueLights == 3);
	CHECK(s.uniqueLightsInScene == 2);  // light 1 is in a scene list through one of its entries
	CHECK(s.complete);
}

TEST_CASE("an empty result proves nothing when a root or node is missing")
{
	CHECK(!Summarize(BothViews(true, false), {}).complete);                               // first-person node not found
	CHECK(!Summarize(BothViews(true, false, false), {}).complete);                        // first-person 3D searched but not loaded
	CHECK(!Summarize(BothViews(false, true), {}).complete);                               // third-person node not found
	CHECK(!Summarize({ Search{ "thirdPerson", "WEAPON", false, false } }, {}).complete);  // third-person 3D not loaded
	CHECK(!Summarize({}, {}).complete);                                                   // nothing searched
	const auto none = Summarize(BothViews(), {});
	CHECK(none.complete);  // both nodes searched: no lights is a real result
	CHECK(none.entries == 0);
}

TEST_CASE("an actor searched in its third-person view only is complete on it")
{
	// an NPC: no first-person tree of its own, so only thirdPerson is searched
	CHECK(Summarize({ Search{ "thirdPerson", "WEAPON", true, true } }, {}).complete);
	CHECK(!Summarize({ Search{ "thirdPerson", "WEAPON", true, false } }, {}).complete);  // its node is still needed
}

TEST_CASE("scene list names map to membership")
{
	CHECK(MembershipOf(nullptr) == Membership::kNone);
	CHECK(MembershipOf("active") == Membership::kActive);
	CHECK(MembershipOf("shadow") == Membership::kShadow);
}

TEST_CASE("the hand's held-light summary fields")
{
	const auto searches = BothViews();
	const auto summary = Summarize(searches, { { 7, Membership::kActive }, { 7, Membership::kNone } });
	const auto j = SummaryFields(searches, summary, false);
	CHECK(j.size() == 5);
	CHECK(j["heldLightEntriesInScene"] == 1);
	CHECK(j["heldLightsUnique"] == 1);
	CHECK(j["heldLightsUniqueInScene"] == 1);
	CHECK(j["heldLightSearchComplete"] == true);
	CHECK(!j.contains("heldLightsRendered"));
	CHECK(j["heldLightSearch"].size() == 2);
	const auto& first = j["heldLightSearch"][1];
	CHECK(first["view"] == "firstPerson");
	CHECK(first["node"] == "WEAPON");
	CHECK(first["match"] == "firstByName");
	CHECK(first["rootLoaded"] == true);
	CHECK(first["nodeFound"] == true);
	CHECK(SummaryFields(searches, summary, true)["heldLightSearchComplete"] == false);  // a truncated walk proves nothing
}
