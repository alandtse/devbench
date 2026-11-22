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
	CHECK(ParseLocomotionSpeed(json(nullptr)) == static_cast<float>(kDefaultLocomotionSpeed));
	CHECK(ParseLocomotionSpeed(json(0)) == 0.0f);
	CHECK(ParseLocomotionSpeed(json(250.5)) == 250.5f);
	CHECK(ParseLocomotionSpeed(json(1000)) == 1000.0f);
}

TEST_CASE("a locomotion speed outside the bounds, or not a number, is refused")
{
	CHECK(!ParseLocomotionSpeed(json(-1)).has_value());
	CHECK(!ParseLocomotionSpeed(json(1000.5)).has_value());
	CHECK(!ParseLocomotionSpeed(json("fast")).has_value());
	CHECK(!ParseLocomotionSpeed(json(true)).has_value());
}

TEST_CASE("a gait with no arguments is a plain run")
{
	std::string error;
	const auto  gait = ParseGait(json::object(), error);
	CHECK(gait.has_value());
	if (!gait)
		return;
	CHECK(gait->speed == static_cast<float>(kDefaultLocomotionSpeed));
	CHECK(gait->variables.empty());
	CHECK(gait->startEvents.empty());
	CHECK(gait->stopEvents.empty());
}

TEST_CASE("the sprint shorthand pins the sprint variables and events at a faster default speed")
{
	std::string error;
	const auto  gait = ParseGait(json{ { "sprint", true } }, error);
	CHECK(gait.has_value());
	if (!gait)
		return;
	CHECK(gait->speed == 600.0f);
	CHECK(gait->variables.size() == 2);
	if (gait->variables.size() != 2)
		return;
	CHECK(gait->variables[0].name == "IsSprinting");
	CHECK(std::get<bool>(gait->variables[0].value));
	CHECK(gait->variables[1].name == "iSyncSprintState");
	CHECK(std::get<std::int32_t>(gait->variables[1].value) == 1);
	const std::vector<std::string> start{ "sprintStart" };
	const std::vector<std::string> stop{ "sprintStop" };
	CHECK(gait->startEvents == start);
	CHECK(gait->stopEvents == stop);
}

TEST_CASE("explicit variables and speed override the sprint shorthand")
{
	std::string error;
	const json  args{ { "sprint", true }, { "speed", 450 }, { "variables", json{ { "iSyncSprintState", 2 } } } };
	const auto  gait = ParseGait(args, error);
	CHECK(gait.has_value());
	if (!gait)
		return;
	CHECK(gait->speed == 450.0f);
	CHECK(gait->variables.size() == 2);
	if (gait->variables.size() != 2)
		return;
	CHECK(std::get<std::int32_t>(gait->variables[1].value) == 2);
}

TEST_CASE("variables take a bool, an integer or a float and events are de-duplicated")
{
	std::string error;
	const json  args{ { "variables", json{ { "bSneaking", true }, { "iState", 3 }, { "Direction", 1.5 } } }, { "events", json::array({ "a", "a", "b" }) }, { "stopEvents", json::array({ "c" }) } };
	const auto  gait = ParseGait(args, error);
	CHECK(gait.has_value());
	if (!gait)
		return;
	CHECK(gait->variables.size() == 3);
	if (gait->variables.size() != 3)
		return;
	const auto find = [&](const char* a_name) -> const GraphValue* {
		for (const auto& variable : gait->variables)
			if (variable.name == a_name)
				return &variable.value;
		return nullptr;
	};
	CHECK(find("bSneaking") && std::holds_alternative<bool>(*find("bSneaking")));
	CHECK(find("iState") && std::holds_alternative<std::int32_t>(*find("iState")) && std::get<std::int32_t>(*find("iState")) == 3);
	CHECK(find("Direction") && std::holds_alternative<float>(*find("Direction")) && std::get<float>(*find("Direction")) == 1.5f);
	const std::vector<std::string> start{ "a", "b" };
	const std::vector<std::string> stop{ "c" };
	CHECK(gait->startEvents == start);
	CHECK(gait->stopEvents == stop);
}

TEST_CASE("a malformed gait is refused with a reason")
{
	std::string error;
	const auto  refused = [&](const json& a_args) { return !ParseGait(a_args, error).has_value(); };
	CHECK(refused(json{ { "sprint", "yes" } }));
	CHECK(refused(json{ { "speed", 2000 } }));
	CHECK(refused(json{ { "variables", json::array() } }));
	CHECK(refused(json{ { "variables", json{ { "x", "text" } } } }));
	CHECK(refused(json{ { "variables", json{ { "x", 4294967296LL } } } }));
	CHECK(refused(json{ { "events", "sprintStart" } }));
	CHECK(refused(json{ { "events", json::array({ "" }) } }));
	CHECK(!error.empty());
	json many = json::object();
	for (int i = 0; i < 17; ++i)
		many["v" + std::to_string(i)] = true;
	CHECK(refused(json{ { "variables", many } }));
}

TEST_CASE("a variable returns to its neutral value when the run ends")
{
	const GraphValue flag{ true };
	const GraphValue count{ std::int32_t{ 5 } };
	const GraphValue ratio{ 2.5f };
	CHECK(std::get<bool>(NeutralValue(flag)) == false);
	CHECK(std::get<std::int32_t>(NeutralValue(count)) == 0);
	CHECK(std::get<float>(NeutralValue(ratio)) == 0.0f);
}
