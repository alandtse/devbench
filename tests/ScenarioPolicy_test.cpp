#include "test_framework.h"

#include "ScenarioPolicy.h"

using dvb::json;

TEST_CASE("scenario rejects embedded extension errors")
{
	const json result{
		{ "error", "qualification_wait requires object parameter 'target'" },
		{ "errorCode", "invalid_target" },
	};

	CHECK(dvb::ScenarioPolicy::IsEmbeddedToolFailure(result));
	CHECK(dvb::ScenarioPolicy::EmbeddedToolErrorCode(result) == "invalid_target");
	CHECK(dvb::ScenarioPolicy::EmbeddedToolErrorMessage(result) ==
		  "qualification_wait requires object parameter 'target'");
}

TEST_CASE("scenario rejects explicit ok false receipts")
{
	const json result{ { "ok", false }, { "errors", json::array({ "failed" }) } };

	CHECK(dvb::ScenarioPolicy::IsEmbeddedToolFailure(result));
	CHECK(dvb::ScenarioPolicy::EmbeddedToolErrorCode(result) == 422);
}

TEST_CASE("scenario evaluates ok false after an empty error")
{
	const json result{ { "error", "" }, { "ok", false } };

	CHECK(dvb::ScenarioPolicy::IsEmbeddedToolFailure(result));
	CHECK(dvb::ScenarioPolicy::EmbeddedToolErrorMessage(result) == "tool returned ok:false");
}

TEST_CASE("scenario uses the fallback message for a null error")
{
	const json result{ { "error", nullptr }, { "ok", false } };

	CHECK(dvb::ScenarioPolicy::IsEmbeddedToolFailure(result));
	CHECK(dvb::ScenarioPolicy::EmbeddedToolErrorMessage(result) == "tool returned ok:false");
}

TEST_CASE("semantic waiter outcomes remain successful tool steps")
{
	const json result{
		{ "satisfied", false },
		{ "outcome", "timeout" },
		{ "failureReasons", json::array({ "profile_pending" }) },
	};

	CHECK(!dvb::ScenarioPolicy::IsEmbeddedToolFailure(result));
}

TEST_CASE("a scenario without pose steps leaves recording sampling on")
{
	const json steps = json::array({
		{ { "tool", "record" }, { "args", { { "action", "start" } } } },
		{ { "wait", 2000 } },
		{ { "tool", "console" }, { "args", { { "action", "exec" }, { "command", "player.setpos x 0" } } } },
		{ { "waitUntil", "playerLoaded" } },
		{ { "tool", "record" }, { "args", { { "action", "stop" } } } },
	});

	CHECK(!dvb::ScenarioPolicy::ReplaysTrajectory(steps));
	CHECK(!dvb::ScenarioPolicy::ReplaysTrajectory(json::array()));
	CHECK(!dvb::ScenarioPolicy::ReplaysTrajectory(json{ { "pose", json::array({ 0, 0, 0, 0, 0 }) } }));
}

TEST_CASE("a pose step marks a scenario as a trajectory replay")
{
	const json steps = json::array({
		{ { "tool", "console" }, { "args", { { "action", "exec" }, { "command", "coc QASmoke" } } } },
		{ { "pose", json::array({ 1.0, 2.0, 3.0, 90.0, 0.0 }) }, { "wait", 10 } },
	});

	CHECK(dvb::ScenarioPolicy::ReplaysTrajectory(steps));
}
