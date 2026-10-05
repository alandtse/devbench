#include "test_framework.h"

#include "PapyrusDefaults.h"

using dvb::PapyrusDefaults::Find;
using Kind = dvb::PapyrusDefaults::Value::Kind;

TEST_CASE("papyrus defaults supply PlaceAtMe's count of one")
{
	const auto v = Find("ObjectReference", "PlaceAtMe", "aiCount", 1u);
	CHECK(v.has_value());
	CHECK(v->kind == Kind::kInt);
	CHECK(v->number == 1.0);
	CHECK(v->source == "vanilla");
}

TEST_CASE("papyrus defaults ignore case and know MoveTo matches rotation")
{
	const auto v = Find("objectreference", "moveto", "ABMATCHROTATION", 4u);
	CHECK(v.has_value());
	CHECK(v->kind == Kind::kBool);
	CHECK(v->number == 1.0);
}

TEST_CASE("papyrus defaults leave neutral and unknown parameters to the caller")
{
	CHECK(!Find("ObjectReference", "PlaceAtMe", "abForcePersist", 2u).has_value());
	CHECK(!Find("Actor", "PlaceAtMe", "aiCount", 1u).has_value());
	CHECK(!Find("MyMod", "DoThing", "aiCount", 1u).has_value());
}

TEST_CASE("papyrus defaults need a named parameter's name and position to agree")
{
	CHECK(!Find("ObjectReference", "PlaceAtMe", "aiCount", 2u).has_value());         // right name, wrong position
	CHECK(!Find("ObjectReference", "PlaceAtMe", "abForcePersist", 1u).has_value());  // right position, wrong name
	CHECK(!Find("ObjectReference", "MoveTo", "abMatchRotation", 3u).has_value());    // a signature with another shape
}

TEST_CASE("papyrus defaults match a native function's unnamed parameter by position")
{
	const auto v = Find("ObjectReference", "PlaceAtMe", "param2", 1u);  // how the VM names PlaceAtMe's aiCount
	CHECK(v.has_value());
	CHECK(v->kind == Kind::kInt);
	CHECK(v->number == 1.0);
	CHECK(!Find("ObjectReference", "PlaceAtMe", "param3", 2u).has_value());  // abForcePersist is neutral
}

TEST_CASE("papyrus defaults say which signature a value comes from")
{
	const auto skse = Find("Actor", "EquipItemEx", "equipSound", 3u);
	CHECK(skse.has_value());
	CHECK(skse->source == "SKSE");
	CHECK(!Find("Actor", "SetExpressionOverride", "aiMood", 0u).has_value());  // required: no declared default
	CHECK(Find("Actor", "SetExpressionOverride", "aiStrength", 1u)->number == 100.0);
}

TEST_CASE("papyrus defaults keep EnableFastTravel enabling when called with no argument")
{
	for (const auto* script : { "Game", "ObjectReference" }) {
		const auto v = Find(script, "EnableFastTravel", "param1", 0u);
		CHECK(v.has_value());
		CHECK(v->kind == Kind::kBool);
		CHECK(v->number == 1.0);
		CHECK(v->source == "vanilla");
	}
}

TEST_CASE("papyrus defaults cover every non-neutral default the game's own sources declare")
{
	CHECK(Find("Utility", "RandomInt", "param2", 1u)->number == 100.0);
	CHECK(Find("Game", "PlayBink", "param5", 4u)->number == 1.0);
	CHECK(Find("Quest", "ModObjectiveGlobal", "aiObjectiveID", 2u)->number == -1.0);
	CHECK(Find("Input", "GetMappedKey", "param2", 1u)->source == "SKSE");
	CHECK(Find("Actor", "EquipItemById", "param5", 4u)->source == "SKSE");
}

using dvb::PapyrusDefaults::Choose;
using dvb::PapyrusDefaults::Fill;
using dvb::PapyrusDefaults::ParamType;

TEST_CASE("an omitted argument the table does not know refuses the call unless neutral values are allowed")
{
	CHECK(Choose(std::nullopt, ParamType::kBool, false) == Fill::kRefuse);
	CHECK(Choose(std::nullopt, ParamType::kOther, false) == Fill::kRefuse);
	CHECK(Choose(std::nullopt, ParamType::kInt, true) == Fill::kNeutral);
}

TEST_CASE("a table value is used only when its kind matches the parameter's type")
{
	const auto count = Find("ObjectReference", "PlaceAtMe", "aiCount", 1u);
	CHECK(Choose(count, ParamType::kInt, false) == Fill::kTable);
	CHECK(Choose(count, ParamType::kFloat, false) == Fill::kRefuse);
	CHECK(Choose(count, ParamType::kOther, true) == Fill::kNeutral);
}
