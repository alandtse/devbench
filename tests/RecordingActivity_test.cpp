#include "test_framework.h"

#include "RecordingActivity.h"
#include "VRInputState.h"

using dvb::json;
using dvb::Recording::ActivityCaptureContract;
using dvb::Recording::BuildVRTrackedSetReplay;
using dvb::Recording::InterleaveReplayableActivity;
using dvb::Recording::SummarizeActivity;

namespace
{
	json Button(std::int64_t a_ms, std::uint64_t a_seq, const char* a_device,
		const char* a_state, int a_id)
	{
		return json{ { "kind", "input" }, { "eventType", "button" },
			{ "device", a_device }, { "state", a_state }, { "idCode", a_id },
			{ "tMs", a_ms }, { "seq", a_seq } };
	}
}

TEST_CASE("activity contract declares capture-all and partial replay honestly")
{
	const json contract = ActivityCaptureContract();
	CHECK(contract["version"]["major"] == 1);
	CHECK(contract["inputLayer"] == "Skyrim.BSInputDeviceManager");
	CHECK(contract["replay"]["keyboardButtonTransitions"] == true);
	CHECK(contract["replay"]["vrTrackedInputFrames"] == true);
	CHECK(contract["replay"]["controllerButtonTransitions"] == true);
}

TEST_CASE("activity summary separates replayable keyboard transitions from preserved input")
{
	const json events = json::array({
		Button(10, 1, "keyboard", "down", 17),
		Button(20, 2, "keyboard", "held", 17),
		Button(30, 3, "oculusSecondary", "down", 33),
		json{ { "kind", "menu" }, { "name", "InventoryMenu" }, { "opening", true } },
	});
	const json summary = SummarizeActivity(events);
	CHECK(summary["total"] == 4);
	CHECK(summary["input"] == 3);
	CHECK(summary["menu"] == 1);
	CHECK(summary["keyboardTransitions"] == 1);
	CHECK(summary["vrControllerEvents"] == 1);
	CHECK(summary["unsupportedInput"] == 1);
}

TEST_CASE("legacy controller events become atomic tracked-set frames without losing short presses")
{
	const auto pose = [](int index) {
		return json{ { "available", true }, { "connected", true }, { "valid", true },
			{ "index", index }, { "trackingResult", 200 },
			{ "matrix", json::array({ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 }) },
			{ "velocity", json::array({ 0, 0, 0 }) },
			{ "angularVelocity", json::array({ 0, 0, 0 }) } };
	};
	const json samples = json::array({
		json{ { "tMs", 20 }, { "originCode", 1 }, { "hmd", pose(0) },
			{ "left", pose(1) }, { "right", pose(2) } },
		json{ { "tMs", 100 }, { "originCode", 1 }, { "hmd", pose(0) },
			{ "left", pose(1) }, { "right", pose(2) } },
	});
	json       down = Button(40, 1, "oculusPrimary", "down", 33);
	down["wandIndex"] = 2;
	json up = Button(60, 2, "oculusPrimary", "up", 33);
	up["wandIndex"] = 2;
	const json plan = BuildVRTrackedSetReplay(samples, json::array({ down, up }),
		"recording:test", true);

	CHECK(plan["step"]["tool"] == "input");
	CHECK(plan["step"]["args"]["device"] == "vrTrackedSet");
	CHECK(plan["step"]["args"]["surviveLifecycle"] == true);
	const auto& frames = plan["step"]["args"]["frames"];
	CHECK(frames.size() == 5);
	CHECK(frames[0]["tMs"] == 0);
	CHECK(frames[2]["tMs"] == 40);
	CHECK(frames[2]["right"]["controller"]["pressed"].get<std::uint64_t>() == (std::uint64_t{ 1 } << 33));
	CHECK(frames[3]["tMs"] == 60);
	CHECK(frames[3]["right"]["controller"]["pressed"] == 0);
	CHECK(plan["durationMs"] == 150);
	CHECK(plan["step"]["args"]["tailMs"] == 50);
	CHECK(plan["report"]["convertedControllerEvents"] == 2);
	CHECK(plan["report"]["roleFallbackEvents"] == 0);

	const json fallbackPlan = BuildVRTrackedSetReplay(samples,
		json::array({ Button(40, 3, "oculusPrimary", "down", 33) }), "recording:test", true);
	CHECK(fallbackPlan["report"]["roleFallbackEvents"] == 1);
	CHECK(fallbackPlan["step"]["args"]["frames"][2]["right"]["controller"]["pressed"].get<std::uint64_t>() ==
		  (std::uint64_t{ 1 } << 33));
}

TEST_CASE("VR tracked-set replay rejects malformed source samples before returning steps")
{
	json malformed = json::array({ json{ { "tMs", 20 }, { "hmd", json::object() } } });
	CHECK_THROWS(BuildVRTrackedSetReplay(malformed, json::array(), "recording:test", true));

	json duplicate = json::array({ json{ { "tMs", 20 } }, json{ { "tMs", 20 } } });
	CHECK_THROWS(BuildVRTrackedSetReplay(duplicate, json::array(), "recording:test", true));

	json overLimit = json::array({ json{ { "tMs", dvb::kMaximumVRTrackedDurationMs + 1 } } });
	CHECK_THROWS_AS(BuildVRTrackedSetReplay(overLimit, json::array(), "recording:test", true),
		std::invalid_argument);

	json atLimit = json::array({ json{ { "tMs", dvb::kMaximumVRTrackedDurationMs } } });
	json collision = Button(dvb::kMaximumVRTrackedDurationMs, 1, "oculusPrimary", "down", 33);
	CHECK_THROWS_AS(BuildVRTrackedSetReplay(atLimit, json::array({ collision }),
						"recording:test", true),
		std::invalid_argument);
}

TEST_CASE("canonical VR replay enforces its final frame budget")
{
	json samples = json::array();
	for (std::int64_t i = 1; i <= dvb::kMaximumVRTrackedFrames; ++i)
		samples.push_back(json{ { "tMs", i } });
	CHECK_THROWS_AS(BuildVRTrackedSetReplay(samples, json::array(),
						"recording:test", true),
		std::invalid_argument);

	// With time zero already present, one controller transition would become frame 60,001.
	samples[0]["tMs"] = 0;
	json controller = Button(1, 1, "oculusPrimary", "down", 33);
	controller["wandIndex"] = 2;
	CHECK_THROWS_AS(BuildVRTrackedSetReplay(samples, json::array({ controller }),
						"recording:test", true),
		std::invalid_argument);
}

TEST_CASE("keyboard replay planning rejects wrong-typed ordering fields")
{
	const json badTime = json::array({
		json{ { "kind", "input" }, { "eventType", "button" }, { "device", "keyboard" },
			{ "state", "down" }, { "idCode", 30 }, { "tMs", "now" }, { "seq", 1 } },
	});
	CHECK_THROWS_AS(InterleaveReplayableActivity(json::array(), badTime, "recording:test", true),
		json::type_error);

	const json badSequence = json::array({
		json{ { "kind", "input" }, { "eventType", "button" }, { "device", "keyboard" },
			{ "state", "down" }, { "idCode", 30 }, { "tMs", 1 }, { "seq", "first" } },
		json{ { "kind", "input" }, { "eventType", "button" }, { "device", "keyboard" },
			{ "state", "up" }, { "idCode", 30 }, { "tMs", 1 }, { "seq", 2 } },
	});
	CHECK_THROWS_AS(InterleaveReplayableActivity(json::array(), badSequence, "recording:test", true),
		json::type_error);
}

TEST_CASE("same-millisecond controller transitions remain distinct atomic frames")
{
	const auto pose = [](int index) {
		return json{ { "available", true }, { "connected", true }, { "valid", true },
			{ "index", index }, { "trackingResult", 200 },
			{ "matrix", json::array({ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 }) },
			{ "velocity", json::array({ 0, 0, 0 }) },
			{ "angularVelocity", json::array({ 0, 0, 0 }) } };
	};
	const json samples = json::array({
		json{ { "tMs", 10 }, { "originCode", 1 }, { "hmd", pose(0) },
			{ "left", pose(1) }, { "right", pose(2) } },
		json{ { "tMs", 20 }, { "originCode", 1 }, { "hmd", pose(0) },
			{ "left", pose(1) }, { "right", pose(2) } },
	});
	json       down = Button(10, 1, "oculusPrimary", "down", 33);
	down["wandIndex"] = 2;
	json up = Button(10, 2, "oculusPrimary", "up", 33);
	up["wandIndex"] = 2;

	const json  plan = BuildVRTrackedSetReplay(samples, json::array({ down, up }),
		"recording:test", true);
	const auto& frames = plan["step"]["args"]["frames"];
	CHECK(frames[2]["tMs"] == 11);
	CHECK(frames[2]["right"]["controller"]["pressed"].get<std::uint64_t>() ==
		  (std::uint64_t{ 1 } << 33));
	CHECK(frames[3]["tMs"] == 12);
	CHECK(frames[3]["right"]["controller"]["pressed"] == 0);
	CHECK(plan["report"]["timestampAdjustedControllerEvents"] == 2);
}

TEST_CASE("trajectory atMs preserves an initial no-player recording delay")
{
	const json   steps = json::array({
		json{ { "atMs", 125 }, { "pose", json::array({ 1, 2, 3, 4, 5 }) }, { "wait", 25 } },
	});
	const json   plan = InterleaveReplayableActivity(steps, json::array(), "recording:test", true);
	std::int64_t totalWait = 0;
	for (const auto& step : plan["steps"])
		if (step.contains("wait"))
			totalWait += step["wait"].get<std::int64_t>();
	CHECK(totalWait == 150);
}

TEST_CASE("keyboard transitions interleave without changing the original trajectory clock")
{
	const json steps = json::array({
		json{ { "pose", json::array({ 1, 2, 3, 4, 5 }) }, { "wait", 100 } },
	});
	const json events = json::array({
		Button(25, 1, "keyboard", "down", 17),
		Button(50, 2, "keyboard", "held", 17),
		Button(60, 3, "oculusSecondary", "down", 33),
		Button(75, 4, "keyboard", "up", 17),
	});
	const json plan = InterleaveReplayableActivity(steps, events, "recording:test", true);
	CHECK(plan["report"]["replayedKeyboardTransitions"] == 2);
	CHECK(plan["report"]["skippedInput"] == 2);
	CHECK(plan["report"]["partial"] == true);

	std::int64_t totalWait = 0;
	json         transitions = json::array();
	for (const auto& step : plan["steps"]) {
		if (step.contains("wait"))
			totalWait += step["wait"].get<std::int64_t>();
		if (step.value("tool", std::string{}) == "input")
			transitions.push_back(step["args"]["action"]);
	}
	CHECK(totalWait == 100);
	CHECK(transitions == json::array({ "down", "up" }));
	CHECK(plan["inputOwner"] == "recording:test");
	const auto downStep = std::find_if(plan["steps"].begin(), plan["steps"].end(),
		[](const json& step) {
			return step.value("tool", std::string{}) == "input" &&
		           step["args"].value("action", std::string{}) == "down";
		});
	CHECK(downStep != plan["steps"].end());
	if (downStep != plan["steps"].end())
		CHECK((*downStep)["args"]["maxHoldMs"] == 2050);
}

TEST_CASE("keyboard replay rejects holds without a complete safety margin")
{
	const json atLimit = json::array({
		Button(0, 1, "keyboard", "down", 17),
		Button(58000, 2, "keyboard", "up", 17),
	});
	const json plan = InterleaveReplayableActivity(json::array(), atLimit,
		"recording:test", true);
	CHECK(plan["steps"][0]["args"]["maxHoldMs"] == 60000);

	const json overLimit = json::array({
		Button(0, 1, "keyboard", "down", 17),
		Button(58001, 2, "keyboard", "up", 17),
	});
	CHECK_THROWS_AS(InterleaveReplayableActivity(json::array(), overLimit,
						"recording:test", true),
		std::invalid_argument);
}

TEST_CASE("input replay can be disabled without altering legacy steps")
{
	const json steps = json::array({ json{ { "wait", 40 } } });
	const json events = json::array({ Button(10, 1, "keyboard", "down", 17) });
	const json plan = InterleaveReplayableActivity(steps, events, "recording:test", false);
	CHECK(plan["steps"] == steps);
	CHECK(plan["report"]["enabled"] == false);
	CHECK(plan["report"]["replayedKeyboardTransitions"] == 0);
	CHECK(plan["inputOwner"] == "");
}
