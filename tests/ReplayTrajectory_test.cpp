#include "test_framework.h"

#include "ReplayTrajectory.h"

#include <cmath>

using dvb::json;
using dvb::Recording::ExtractKeyframes;
using dvb::Recording::Interpolation;
using dvb::Recording::Pose;
using dvb::Recording::PoseKeyframe;
using dvb::Recording::ShortestArcDeltaDeg;
using dvb::Recording::Trajectory;

namespace
{
	bool Near(double a_a, double a_b, double a_eps = 1e-6)
	{
		return std::fabs(a_a - a_b) <= a_eps;
	}

	PoseKeyframe Key(std::int64_t a_ms, double a_x, double a_yaw = 0.0, double a_pitch = 0.0,
		double a_y = 0.0, double a_z = 0.0)
	{
		return { a_ms, Pose{ a_x, a_y, a_z, a_yaw, a_pitch } };
	}
}

TEST_CASE("keyframes sit at atMs, or at the summed waits for legacy recordings")
{
	const json withAt = json::array({
		json{ { "atMs", 100 }, { "pose", json::array({ 1, 2, 3, 4, 5 }) }, { "wait", 10 } },
		json{ { "atMs", 120 }, { "pose", json::array({ 2, 2, 3, 4, 5 }) }, { "wait", 10 } },
	});
	const auto keys = ExtractKeyframes(withAt);
	CHECK(keys.size() == 2);
	CHECK(keys[0].tMs == 100);
	CHECK(keys[1].tMs == 120);

	const json legacy = json::array({
		json{ { "pose", json::array({ 0, 0, 0, 0, 0 }) }, { "wait", 10 } },
		json{ { "wait", 5 } },
		json{ { "pose", json::array({ 1, 0, 0, 0, 0 }) }, { "wait", 10 } },
	});
	const auto legacyKeys = ExtractKeyframes(legacy);
	CHECK(legacyKeys.size() == 2);
	CHECK(legacyKeys[0].tMs == 0);
	CHECK(legacyKeys[1].tMs == 15);
}

TEST_CASE("keyframe extraction skips malformed poses and non-pose steps")
{
	const json steps = json::array({
		json{ { "tool", "console" }, { "atMs", 5 } },
		json{ { "pose", json::array({ 1, 2, 3 }) }, { "wait", 10 } },
		json{ { "pose", json::array({ "a", 2, 3, 4, 5 }) }, { "wait", 10 } },
		json{ { "pose", json::array({ 1, 2, 3, 4, 5 }) }, { "atMs", 40 } },
	});
	const auto keys = ExtractKeyframes(steps);
	CHECK(keys.size() == 1);
	CHECK(keys[0].tMs == 40);
	CHECK(ExtractKeyframes(json::object()).empty());
}

TEST_CASE("a repeated timestamp keeps the later pose")
{
	const json steps = json::array({
		json{ { "atMs", 10 }, { "pose", json::array({ 1, 0, 0, 0, 0 }) } },
		json{ { "atMs", 10 }, { "pose", json::array({ 9, 0, 0, 0, 0 }) } },
	});
	const auto keys = ExtractKeyframes(steps);
	CHECK(keys.size() == 1);
	CHECK(Near(keys[0].pose.x, 9.0));
}

TEST_CASE("sampling hits keyframes exactly and blends linearly between them")
{
	const Trajectory t({ Key(0, 0.0, 0.0, 0.0), Key(100, 10.0, 90.0, 20.0) });
	CHECK(Near(t.Sample(0).x, 0.0));
	CHECK(Near(t.Sample(100).x, 10.0));
	const Pose mid = t.Sample(50);
	CHECK(Near(mid.x, 5.0));
	CHECK(Near(mid.yawDeg, 45.0));
	CHECK(Near(mid.pitchDeg, 10.0));
	CHECK(t.StartMs() == 0);
	CHECK(t.EndMs() == 100);
}

TEST_CASE("sampling clamps outside the recorded clock and tolerates an empty trajectory")
{
	const Trajectory t({ Key(100, 1.0), Key(200, 2.0) });
	CHECK(Near(t.Sample(-50).x, 1.0));
	CHECK(Near(t.Sample(1000).x, 2.0));
	const Trajectory empty({});
	CHECK(empty.Empty());
	CHECK(Near(empty.Sample(10).x, 0.0));
}

TEST_CASE("yaw takes the shortest arc across the 0/360 seam")
{
	CHECK(Near(ShortestArcDeltaDeg(359.0, 1.0), 2.0));
	CHECK(Near(ShortestArcDeltaDeg(1.0, 359.0), -2.0));
	CHECK(Near(ShortestArcDeltaDeg(0.0, 180.0), 180.0));

	const Trajectory t({ Key(0, 0.0, 359.0), Key(100, 0.0, 1.0) });
	CHECK(Near(t.Sample(50).yawDeg, 0.0));
	const double quarter = t.Sample(25).yawDeg;
	CHECK(Near(quarter, 359.5));
	CHECK(t.Sample(75).yawDeg >= 0.0 && t.Sample(75).yawDeg < 360.0);
}

TEST_CASE("a teleport gap holds then snaps instead of sliding across the map")
{
	const Trajectory t({ Key(0, 0.0), Key(100, 50000.0), Key(200, 50010.0) });
	CHECK(Near(t.Sample(60).x, 0.0));
	CHECK(Near(t.Sample(99.9).x, 0.0));
	CHECK(Near(t.Sample(100).x, 50000.0));
	CHECK(Near(t.Sample(150).x, 50005.0));
}

TEST_CASE("pose is a function of absolute time, independent of frame delivery")
{
	std::vector<PoseKeyframe> keys;
	for (int i = 0; i <= 100; ++i)
		keys.push_back(Key(i * 10, i * 3.0, std::fmod(i * 7.0, 360.0), i * 0.1));
	const Trajectory t(keys);

	const double steady[] = { 0, 16, 32, 48, 64, 80, 96, 112, 128, 144 };
	const double bursty[] = { 0, 3, 9, 48, 51, 64, 66, 101, 144, 400 };
	for (const double ms : steady)
		CHECK(Near(t.Sample(ms).x, std::min(ms, 1000.0) * 0.3, 1e-9));
	for (const double ms : bursty)
		CHECK(Near(t.Sample(ms).x, std::min(ms, 1000.0) * 0.3, 1e-9));

	double previous = -1.0;
	for (double ms = 0; ms <= 1000; ms += 1.7) {
		const double x = t.Sample(ms).x;
		CHECK(x >= previous);
		previous = x;
	}
}

TEST_CASE("catmull-rom passes through keyframes and stays inside a monotone path")
{
	std::vector<PoseKeyframe> keys;
	for (int i = 0; i < 6; ++i)
		keys.push_back(Key(i * 50, i * i * 4.0));
	const Trajectory spline(keys, Interpolation::CatmullRom);
	for (const auto& key : keys)
		CHECK(Near(spline.Sample(static_cast<double>(key.tMs)).x, key.pose.x, 1e-9));

	double previous = -1e9;
	for (double ms = 0; ms <= 250; ms += 5.0) {
		const double x = spline.Sample(ms).x;
		CHECK(x >= previous - 1e-9);
		previous = x;
	}
}

TEST_CASE("catmull-rom does not overshoot into a teleport")
{
	const Trajectory t({ Key(0, 0.0), Key(50, 10.0), Key(100, 90000.0), Key(150, 90010.0) },
		Interpolation::CatmullRom);
	for (double ms = 50; ms < 100; ms += 1.0) {
		const double x = t.Sample(ms).x;
		CHECK(x >= 10.0 - 1e-9);
		CHECK(x <= 10.0 + 1e-9);
	}
}

TEST_CASE("unsorted keyframes are ordered and duplicates collapse")
{
	const Trajectory t({ Key(100, 10.0), Key(0, 0.0), Key(100, 20.0) });
	CHECK(t.StartMs() == 0);
	CHECK(t.EndMs() == 100);
	CHECK(Near(t.Sample(100).x, 20.0));
}

TEST_CASE("legacy waits scale to the recorded duration without drifting")
{
	json steps = json::array();
	for (int i = 0; i < 1000; ++i)
		steps.push_back(json{ { "pose", json::array({ i, 0, 0, 0, 0 }) }, { "wait", 10 } });
	const json   scaled = dvb::Recording::ScaleWaitsToRecordedDuration(steps, 26500);
	std::int64_t total = 0;
	for (const auto& step : scaled)
		total += step["wait"].get<std::int64_t>();
	CHECK(total == 26500);
	CHECK(scaled[0]["pose"] == steps[0]["pose"]);
	const auto keys = ExtractKeyframes(scaled);
	CHECK(keys.size() == 1000);
	CHECK(keys.back().tMs >= 26000);
}

TEST_CASE("wait scaling leaves atMs recordings and unusable durations alone")
{
	const json withAt = json::array({
		json{ { "atMs", 0 }, { "pose", json::array({ 0, 0, 0, 0, 0 }) }, { "wait", 10 } },
		json{ { "atMs", 10 }, { "pose", json::array({ 1, 0, 0, 0, 0 }) }, { "wait", 10 } },
	});
	CHECK(dvb::Recording::ScaleWaitsToRecordedDuration(withAt, 5000) == withAt);

	const json legacy = json::array({ json{ { "pose", json::array({ 0, 0, 0, 0, 0 }) }, { "wait", 10 } } });
	CHECK(dvb::Recording::ScaleWaitsToRecordedDuration(legacy, 0) == legacy);
	CHECK(dvb::Recording::ScaleWaitsToRecordedDuration(legacy, 10) == legacy);  // never shortens
	CHECK(dvb::Recording::ScaleWaitsToRecordedDuration(json::object(), 5000) == json::object());
}
