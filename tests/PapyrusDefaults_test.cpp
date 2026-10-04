#include "test_framework.h"

#include "PapyrusDefaults.h"

using dvb::PapyrusDefaults::Find;
using Kind = dvb::PapyrusDefaults::Value::Kind;

TEST_CASE("papyrus defaults supply PlaceAtMe's count of one")
{
	const auto v = Find("ObjectReference", "PlaceAtMe", "aiCount");
	CHECK(v.has_value());
	CHECK(v->kind == Kind::kInt);
	CHECK(v->number == 1.0);
}

TEST_CASE("papyrus defaults ignore case and know MoveTo matches rotation")
{
	const auto v = Find("objectreference", "moveto", "ABMATCHROTATION");
	CHECK(v.has_value());
	CHECK(v->kind == Kind::kBool);
	CHECK(v->number == 1.0);
}

TEST_CASE("papyrus defaults leave neutral and unknown parameters to the caller")
{
	CHECK(!Find("ObjectReference", "PlaceAtMe", "abForcePersist").has_value());
	CHECK(!Find("Actor", "PlaceAtMe", "aiCount").has_value());
	CHECK(!Find("MyMod", "DoThing", "aiCount").has_value());
}

TEST_CASE("papyrus defaults match a native function's unnamed parameter by position")
{
	const auto v = Find("ObjectReference", "PlaceAtMe", "param2", 1u);  // how the VM names PlaceAtMe's aiCount
	CHECK(v.has_value());
	CHECK(v->kind == Kind::kInt);
	CHECK(v->number == 1.0);
	CHECK(!Find("ObjectReference", "PlaceAtMe", "param3", 2u).has_value());          // abForcePersist is neutral
	CHECK(!Find("ObjectReference", "PlaceAtMe", "param2").has_value());              // no position: by name only
	CHECK(!Find("ObjectReference", "PlaceAtMe", "abForcePersist", 1u).has_value());  // a real name never matches by position
}

TEST_CASE("papyrus defaults keep EnableFastTravel enabling when called with no argument")
{
	for (const auto* script : { "Game", "ObjectReference" }) {
		const auto v = Find(script, "EnableFastTravel", "param1", 0u);
		CHECK(v.has_value());
		CHECK(v->kind == Kind::kBool);
		CHECK(v->number == 1.0);
	}
}

TEST_CASE("papyrus defaults cover every non-neutral default the game's own sources declare")
{
	CHECK(Find("Utility", "RandomInt", "param2", 1u)->number == 100.0);
	CHECK(Find("Input", "GetMappedKey", "param2", 1u)->number == 255.0);
	CHECK(Find("Quest", "ModObjectiveGlobal", "aiObjectiveID", 2u)->number == -1.0);
	CHECK(Find("Actor", "KeepOffsetFromActor", "param9", 8u)->kind == Kind::kFloat);
}
