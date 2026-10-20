#include "test_framework.h"

#include "ScenarioPolicy.h"

using dvb::json;

TEST_CASE("a replay holds the free camera unless it injects a VR tracked set on VR")
{
	const json injected{ { "enabled", true }, { "emittedFrames", 488 } };
	CHECK(dvb::ScenarioPolicy::ShouldHoldReplayCamera(json::object(), true));
	CHECK(dvb::ScenarioPolicy::ShouldHoldReplayCamera(json(nullptr), true));
	CHECK(dvb::ScenarioPolicy::ShouldHoldReplayCamera(json{ { "enabled", true }, { "emittedFrames", 0 } }, true));
	CHECK(dvb::ScenarioPolicy::ShouldHoldReplayCamera(json{ { "emittedFrames", "many" } }, true));
	CHECK(!dvb::ScenarioPolicy::ShouldHoldReplayCamera(injected, true));
}

TEST_CASE("a recorded tracked set is not injected off VR, so the camera is still held")
{
	const json injected{ { "enabled", true }, { "emittedFrames", 488 } };
	CHECK(dvb::ScenarioPolicy::ShouldHoldReplayCamera(injected, false));
}

TEST_CASE("an explicit holdCamera overrides the choice either way")
{
	const json injected{ { "enabled", true }, { "emittedFrames", 488 } };
	CHECK(dvb::ScenarioPolicy::ShouldHoldReplayCamera(injected, true, true));
	CHECK(!dvb::ScenarioPolicy::ShouldHoldReplayCamera(json::object(), false, false));
}

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

namespace
{
	json ConsoleStep(const char* a_command)
	{
		return json{ { "tool", "console" }, { "args", { { "action", "exec" }, { "command", a_command } } } };
	}

	json PoseStep(long a_waitMs)
	{
		return json{ { "pose", json::array({ 1.0, 2.0, 3.0, 90.0, 0.0 }) }, { "wait", a_waitMs } };
	}
}

TEST_CASE("a scenario without trajectory steps leaves recording sampling on")
{
	const json steps = json::array({
		{ { "tool", "record" }, { "args", { { "action", "start" } } } },
		{ { "wait", 2000 } },
		{ { "tool", "input" }, { "args", { { "action", "press" }, { "device", "keyboard" }, { "key", "w" } } } },
		ConsoleStep("coc QASmoke"),
		{ { "waitUntil", "playerLoaded" } },
		{ { "tool", "record" }, { "args", { { "action", "stop" } } } },
	});

	CHECK(!dvb::ScenarioPolicy::TrajectorySpan(steps));
	CHECK(!dvb::ScenarioPolicy::TrajectorySpan(json::array()));
	CHECK(!dvb::ScenarioPolicy::TrajectorySpan(json{ { "pose", json::array({ 0, 0, 0, 0, 0 }) } }));
}

TEST_CASE("pose rows and player.setpos/setangle console steps are trajectory steps")
{
	CHECK(dvb::ScenarioPolicy::IsTrajectoryStep(PoseStep(10)));
	CHECK(dvb::ScenarioPolicy::IsTrajectoryStep(ConsoleStep("player.setpos x 0")));
	CHECK(dvb::ScenarioPolicy::IsTrajectoryStep(ConsoleStep("  Player.SetAngle z 90")));
	CHECK(!dvb::ScenarioPolicy::IsTrajectoryStep(ConsoleStep("player.setposition x 0")));
	CHECK(!dvb::ScenarioPolicy::IsTrajectoryStep(ConsoleStep("player.setav health 100")));
	CHECK(!dvb::ScenarioPolicy::IsTrajectoryStep(ConsoleStep("player.setpos")));
	CHECK(!dvb::ScenarioPolicy::IsTrajectoryStep(json{ { "tool", "papyrus" }, { "args", { { "command", "player.setpos x 0" } } } }));
	CHECK(!dvb::ScenarioPolicy::IsTrajectoryStep(json{ { "wait", 10 } }));
}

TEST_CASE("the replay mark covers only a mixed scenario's trajectory span")
{
	const json steps = json::array({
		{ { "tool", "record" }, { "args", { { "action", "start" } } } },
		{ { "wait", 2000 } },
		PoseStep(10),
		{ { "wait", 10 } },
		PoseStep(10),
		{ { "wait", 25 } },
		{ { "tool", "input" }, { "args", { { "action", "press" }, { "device", "keyboard" }, { "key", "w" } } } },
		{ { "wait", 2000 } },
		{ { "tool", "record" }, { "args", { { "action", "stop" } } } },
	});

	const auto span = dvb::ScenarioPolicy::TrajectorySpan(steps);
	CHECK(span.has_value());
	if (!span)
		return;
	CHECK(span->first == 2);
	CHECK(span->last == 5);
}

TEST_CASE("a step between two poses releases the replay mark, the poses and bare waits hold it")
{
	const json input{ { "tool", "input" }, { "args", { { "action", "tap" }, { "device", "keyboard" }, { "key", "w" } } } };
	const json steps = json::array({ PoseStep(10), input, { { "wait", 10 } }, PoseStep(10) });

	const auto span = dvb::ScenarioPolicy::TrajectorySpan(steps);
	CHECK(span.has_value());
	if (!span)
		return;
	CHECK(span->first == 0);
	CHECK(span->last == 3);
	CHECK(dvb::ScenarioPolicy::HoldsReplayMark(steps[0]));
	CHECK(!dvb::ScenarioPolicy::HoldsReplayMark(steps[1]));
	CHECK(dvb::ScenarioPolicy::HoldsReplayMark(steps[2]));
	CHECK(dvb::ScenarioPolicy::HoldsReplayMark(steps[3]));
}

TEST_CASE("a v1 setpos trajectory spans its trailing wait")
{
	const json steps = json::array({
		ConsoleStep("coc QASmoke"),
		{ { "waitUntil", "playerLoaded" } },
		ConsoleStep("player.setpos x 1.00"),
		ConsoleStep("player.setpos y 2.00"),
		ConsoleStep("player.setpos z 3.00"),
		ConsoleStep("player.setangle z 90.00"),
		ConsoleStep("player.setangle x 0.00"),
		{ { "wait", 10 } },
		ConsoleStep("set gamehour to 12"),
	});

	const auto span = dvb::ScenarioPolicy::TrajectorySpan(steps);
	CHECK(span.has_value());
	if (!span)
		return;
	CHECK(span->first == 2);
	CHECK(span->last == 7);
}
