#include "test_framework.h"

#include "ReachLogic.h"

#include <cmath>
#include <optional>
#include <string>

using dvb::json;
using namespace dvb::ReachLogic;

namespace
{

	bool Near(double a_a, double a_b, double a_eps = 1e-9)
	{
		return std::abs(a_a - a_b) <= a_eps;
	}

	bool NearXYZ(const Vec3& a_v, double a_x, double a_y, double a_z, double a_eps = 1e-9)
	{
		return Near(a_v.x, a_x, a_eps) && Near(a_v.y, a_y, a_eps) && Near(a_v.z, a_z, a_eps);
	}

	Vec3 PoseTranslation(const json& a_frame, const char* a_hand)
	{
		const json& matrix = a_frame[a_hand]["matrix"];
		return Vec3{ matrix[3].get<double>(), matrix[7].get<double>(), matrix[11].get<double>() };
	}

	std::optional<ReachRequest> Parse(const std::string& a_json, std::string& a_error)
	{
		try {
			return ParseReach(json::parse(a_json), a_error);
		} catch (...) {
			a_error = "parse threw";
			return std::nullopt;
		}
	}

}

TEST_CASE("Vec3 basics")
{
	Vec3 a{ 1.0, 2.0, 3.0 };
	Vec3 b{ 4.0, 5.0, 6.0 };
	Vec3 sum = Add(a, b);
	CHECK(NearXYZ(sum, 5.0, 7.0, 9.0));
	Vec3 diff = Sub(b, a);
	CHECK(NearXYZ(diff, 3.0, 3.0, 3.0));
	Vec3 scaled = Scale(a, 2.0);
	CHECK(NearXYZ(scaled, 2.0, 4.0, 6.0));
	Vec3 cross = Cross(Vec3{ 1.0, 0.0, 0.0 }, Vec3{ 0.0, 1.0, 0.0 });
	CHECK(NearXYZ(cross, 0.0, 0.0, 1.0));
	CHECK(Near(Dot(a, b), 32.0));
	CHECK(Near(Norm(Vec3{ 3.0, 4.0, 0.0 }), 5.0));
	CHECK(Near(Distance(Vec3{ 0.0, 0.0, 0.0 }, Vec3{ 3.0, 4.0, 0.0 }), 5.0));
	Vec3 zeroUnit = Unit(Vec3{ 0.0, 0.0, 0.0 });
	CHECK(NearXYZ(zeroUnit, 0.0, 0.0, 0.0));
	Vec3 zUnit = Unit(Vec3{ 0.0, 0.0, 5.0 });
	CHECK(NearXYZ(zUnit, 0.0, 0.0, 1.0));
}

TEST_CASE("CalibrationFromPoints recovers per-meter columns")
{
	const double        step = 0.2;
	Vec3                c0{ 68.9, 12.3, 0.0 };
	Vec3                c1{ 0.0, 0.0, 70.0 };
	Vec3                c2{ 12.3, -68.9, 0.0 };
	Vec3                rest{ 100.0, 200.0, 300.0 };
	std::array<Vec3, 4> points;
	points[0] = rest;
	points[1] = Add(rest, Scale(c0, step));
	points[2] = Add(rest, Scale(c1, step));
	points[3] = Add(rest, Scale(c2, step));
	auto calibration = CalibrationFromPoints(points, step);
	if (!calibration) {
		CHECK(false);
		return;
	}
	CHECK(NearXYZ(calibration->perMeter[0], 68.9, 12.3, 0.0));
	CHECK(NearXYZ(calibration->perMeter[1], 0.0, 0.0, 70.0));
	CHECK(NearXYZ(calibration->perMeter[2], 12.3, -68.9, 0.0));
	CHECK(NearXYZ(calibration->rest, 100.0, 200.0, 300.0));
}

TEST_CASE("TrackingFor round trips through the Jacobian")
{
	const double        step = 0.2;
	Vec3                c0{ 68.9, 12.3, 0.0 };
	Vec3                c1{ 0.0, 0.0, 70.0 };
	Vec3                c2{ 12.3, -68.9, 0.0 };
	Vec3                rest{ 100.0, 200.0, 300.0 };
	std::array<Vec3, 4> points;
	points[0] = rest;
	points[1] = Add(rest, Scale(c0, step));
	points[2] = Add(rest, Scale(c1, step));
	points[3] = Add(rest, Scale(c2, step));
	auto calibration = CalibrationFromPoints(points, step);
	if (!calibration) {
		CHECK(false);
		return;
	}
	Vec3 restTracking{ 1.0, 2.0, 3.0 };
	Vec3 world{ 150.0, 250.0, 350.0 };
	Vec3 tracking = TrackingFor(*calibration, restTracking, world);
	Vec3 delta = Sub(tracking, restTracking);
	Vec3 applied = Add(calibration->rest, Add(Scale(calibration->perMeter[0], delta.x), Add(Scale(calibration->perMeter[1], delta.y), Scale(calibration->perMeter[2], delta.z))));
	CHECK(NearXYZ(applied, 150.0, 250.0, 350.0));
}

TEST_CASE("TrackingDelta solves the Jacobian")
{
	Calibration calibration;
	calibration.rest = Vec3{ 0.0, 0.0, 0.0 };
	calibration.perMeter[0] = Vec3{ 68.9, 12.3, 0.0 };
	calibration.perMeter[1] = Vec3{ 0.0, 0.0, 70.0 };
	calibration.perMeter[2] = Vec3{ 12.3, -68.9, 0.0 };
	Vec3 delta{ 100.0, 200.0, 300.0 };
	auto solution = TrackingDelta(calibration, delta);
	if (!solution) {
		CHECK(false);
		return;
	}
	Vec3 applied = Add(Scale(calibration.perMeter[0], solution->x), Add(Scale(calibration.perMeter[1], solution->y), Scale(calibration.perMeter[2], solution->z)));
	CHECK(NearXYZ(applied, 100.0, 200.0, 300.0));
}

TEST_CASE("CalibrationFromPoints rejects singular and non-positive steps")
{
	std::array<Vec3, 4> points;
	points[0] = Vec3{ 0.0, 0.0, 0.0 };
	points[1] = Vec3{ 1.0, 0.0, 0.0 };
	points[2] = Vec3{ 2.0, 0.0, 0.0 };
	points[3] = Vec3{ 0.0, 0.0, 1.0 };
	auto singular = CalibrationFromPoints(points, 1.0);
	CHECK(!singular.has_value());
	auto zeroStep = CalibrationFromPoints(points, 0.0);
	CHECK(!zeroStep.has_value());
	auto negativeStep = CalibrationFromPoints(points, -1.0);
	CHECK(!negativeStep.has_value());
}

TEST_CASE("TrackingDelta rejects singular calibration")
{
	Calibration calibration;
	calibration.rest = Vec3{ 0.0, 0.0, 0.0 };
	calibration.perMeter[0] = Vec3{ 1.0, 0.0, 0.0 };
	calibration.perMeter[1] = Vec3{ 2.0, 0.0, 0.0 };
	calibration.perMeter[2] = Vec3{ 0.0, 0.0, 1.0 };
	Vec3 delta{ 1.0, 2.0, 3.0 };
	auto result = TrackingDelta(calibration, delta);
	CHECK(!result.has_value());
}

TEST_CASE("TrackingFor falls back to rest on singular calibration")
{
	Calibration calibration;
	calibration.rest = Vec3{ 0.0, 0.0, 0.0 };
	calibration.perMeter[0] = Vec3{ 1.0, 0.0, 0.0 };
	calibration.perMeter[1] = Vec3{ 2.0, 0.0, 0.0 };
	calibration.perMeter[2] = Vec3{ 0.0, 0.0, 1.0 };
	Vec3 restTracking{ 7.0, 8.0, 9.0 };
	Vec3 world{ 1.0, 2.0, 3.0 };
	Vec3 result = TrackingFor(calibration, restTracking, world);
	CHECK(NearXYZ(result, 7.0, 8.0, 9.0));
}

TEST_CASE("LookAt produces an orthonormal pose facing the target")
{
	Vec3 position{ 1.0, 2.0, 3.0 };
	Vec3 target{ 4.0, 6.0, 3.0 };
	Pose pose = LookAt(position, target);
	Vec3 forward{ -pose[2], -pose[6], -pose[10] };
	Vec3 expected = Unit(Sub(target, position));
	CHECK(NearXYZ(forward, expected.x, expected.y, expected.z));
	Vec3 right{ pose[0], pose[4], pose[8] };
	Vec3 up{ pose[1], pose[5], pose[9] };
	Vec3 back{ pose[2], pose[6], pose[10] };
	CHECK(Near(Norm(right), 1.0));
	CHECK(Near(Norm(up), 1.0));
	CHECK(Near(Norm(back), 1.0));
	CHECK(Near(Dot(right, up), 0.0));
	CHECK(Near(Dot(right, back), 0.0));
	CHECK(Near(Dot(up, back), 0.0));
	CHECK(Near(pose[3], 1.0));
	CHECK(Near(pose[7], 2.0));
	CHECK(Near(pose[11], 3.0));
}

TEST_CASE("LookAt degenerate inputs stay finite")
{
	Vec3 position{ 0.0, 0.0, 0.0 };
	Pose same = LookAt(position, position);
	for (double value : same) {
		CHECK(std::isfinite(value));
	}
	Vec3 above{ 0.0, 10.0, 0.0 };
	Pose parallel = LookAt(position, above);
	for (double value : parallel) {
		CHECK(std::isfinite(value));
	}
	Vec3 right{ parallel[0], parallel[4], parallel[8] };
	Vec3 up{ parallel[1], parallel[5], parallel[9] };
	Vec3 back{ parallel[2], parallel[6], parallel[10] };
	CHECK(Near(Norm(right), 1.0));
	CHECK(Near(Norm(up), 1.0));
	CHECK(Near(Norm(back), 1.0));
	CHECK(Near(Dot(right, up), 0.0));
	CHECK(Near(Dot(right, back), 0.0));
	CHECK(Near(Dot(up, back), 0.0));
}

TEST_CASE("ParseReach applies defaults")
{
	std::string error;
	auto        request = Parse(R"({"hands":{"left":{"node":"NPC"}}})", error);
	if (!request) {
		CHECK(false);
		return;
	}
	CHECK(request->left.has_value());
	CHECK(request->left->node == "NPC");
	CHECK(Near(request->left->front, 0.0));
	CHECK(NearXYZ(request->left->offset, 0.0, 0.0, 0.0));
	CHECK(!request->right.has_value());
	CHECK(request->head.aim == HeadTarget::Aim::kNone);
	CHECK(!request->head.position.has_value());
	CHECK(request->holdMs == 10000);
	CHECK(request->iterations == 4);
	CHECK(Near(request->toleranceUnits, 1.0));
	CHECK(Near(request->approachUnits, 15.0));
}

TEST_CASE("ParseReach reads every field")
{
	std::string error;
	const char* text = R"({
        "hands": {
            "left": {"node": "LeftNode", "front": 12.5, "offset": [1, 2, 3]},
            "right": {"node": "RightNode", "front": -7.5, "offset": [-1, -2, -3]}
        },
        "head": {"lookAt": "hands", "back": 1.5, "up": 0.75, "pos": [4, 5, 6]},
        "holdMs": 5000,
        "iterations": 7,
        "toleranceUnits": 2.5,
        "approachUnits": 30.0
    })";
	auto        request = Parse(text, error);
	if (!request) {
		CHECK(false);
		return;
	}
	CHECK(request->left.has_value());
	CHECK(request->right.has_value());
	CHECK(request->left->node == "LeftNode");
	CHECK(Near(request->left->front, 12.5));
	CHECK(NearXYZ(request->left->offset, 1.0, 2.0, 3.0));
	CHECK(request->right->node == "RightNode");
	CHECK(Near(request->right->front, -7.5));
	CHECK(NearXYZ(request->right->offset, -1.0, -2.0, -3.0));
	CHECK(request->head.aim == HeadTarget::Aim::kHands);
	CHECK(Near(request->head.back, 1.5));
	CHECK(Near(request->head.up, 0.75));
	CHECK(request->head.position.has_value());
	CHECK(NearXYZ(*request->head.position, 4.0, 5.0, 6.0));
	CHECK(request->holdMs == 5000);
	CHECK(request->iterations == 7);
	CHECK(Near(request->toleranceUnits, 2.5));
	CHECK(Near(request->approachUnits, 30.0));
}

TEST_CASE("ParseReach node lookAt")
{
	std::string error;
	auto        withHand = Parse(R"({"hands":{"right":{"node":"X"}},"head":{"lookAt":"SomeNode"}})", error);
	if (!withHand) {
		CHECK(false);
		return;
	}
	CHECK(withHand->head.aim == HeadTarget::Aim::kNode);
	CHECK(withHand->head.node == "SomeNode");
	auto withoutHand = Parse(R"({"head":{"lookAt":"SomeNode"}})", error);
	CHECK(!withoutHand.has_value());
	CHECK(!error.empty());
}

TEST_CASE("ParseReach rejects missing hands")
{
	std::string error;
	auto        emptyHands = Parse(R"({"hands":{}})", error);
	CHECK(!emptyHands.has_value());
	auto handsAim = Parse(R"({"head":{"lookAt":"hands"}})", error);
	CHECK(!handsAim.has_value());
}

TEST_CASE("ParseReach rejects wrong types")
{
	std::string error;
	auto        nodeNumber = Parse(R"({"hands":{"left":{"node":123}}})", error);
	CHECK(!nodeNumber.has_value());
	CHECK(error.find("node") != std::string::npos);
	auto frontString = Parse(R"({"hands":{"left":{"node":"A","front":"big"}}})", error);
	CHECK(!frontString.has_value());
	CHECK(error.find("front") != std::string::npos);
	auto offsetShort = Parse(R"({"hands":{"left":{"node":"A","offset":[1,2]}}})", error);
	CHECK(!offsetShort.has_value());
	CHECK(error.find("offset") != std::string::npos);
	auto holdFloat = Parse(R"({"hands":{"left":{"node":"A"}},"holdMs":100.5})", error);
	CHECK(!holdFloat.has_value());
	CHECK(error.find("holdMs") != std::string::npos);
	auto handsArray = Parse(R"({"hands":[]})", error);
	CHECK(!handsArray.has_value());
	auto argsArray = Parse(R"([])", error);
	CHECK(!argsArray.has_value());
	auto lookAtNumber = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"lookAt":5}})", error);
	CHECK(!lookAtNumber.has_value());
	auto headArray = Parse(R"({"hands":{"left":{"node":"A"}},"head":[]})", error);
	CHECK(!headArray.has_value());
}

TEST_CASE("ParseReach error messages name the field")
{
	std::string error;
	auto        result = Parse(R"({"hands":{"left":{"node":"A","front":999}}})", error);
	CHECK(!result.has_value());
	CHECK(error.find("hands.left.front") != std::string::npos);
	CHECK(error.find("-500..500") != std::string::npos);
}

TEST_CASE("ParseReach front boundaries")
{
	std::string error;
	auto        minOk = Parse(R"({"hands":{"left":{"node":"A","front":-500}}})", error);
	CHECK(minOk.has_value());
	auto maxOk = Parse(R"({"hands":{"left":{"node":"A","front":500}}})", error);
	CHECK(maxOk.has_value());
	auto belowMin = Parse(R"({"hands":{"left":{"node":"A","front":-500.1}}})", error);
	CHECK(!belowMin.has_value());
	auto aboveMax = Parse(R"({"hands":{"left":{"node":"A","front":500.1}}})", error);
	CHECK(!aboveMax.has_value());
}

TEST_CASE("ParseReach offset boundaries")
{
	std::string error;
	auto        maxOk = Parse(R"({"hands":{"left":{"node":"A","offset":[1000,-1000,1000]}}})", error);
	CHECK(maxOk.has_value());
	auto tooBig = Parse(R"({"hands":{"left":{"node":"A","offset":[1000.1,0,0]}}})", error);
	CHECK(!tooBig.has_value());
	auto tooSmall = Parse(R"({"hands":{"left":{"node":"A","offset":[0,0,-1000.1]}}})", error);
	CHECK(!tooSmall.has_value());
}

TEST_CASE("ParseReach head back and up boundaries")
{
	std::string error;
	auto        backMin = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"lookAt":"hands","back":0}})", error);
	CHECK(backMin.has_value());
	auto backMax = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"lookAt":"hands","back":5}})", error);
	CHECK(backMax.has_value());
	auto backLow = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"back":-0.1}})", error);
	CHECK(!backLow.has_value());
	auto backHigh = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"back":5.1}})", error);
	CHECK(!backHigh.has_value());
	auto upMin = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"lookAt":"hands","up":0}})", error);
	CHECK(upMin.has_value());
	auto upMax = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"lookAt":"hands","up":5}})", error);
	CHECK(upMax.has_value());
	auto upLow = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"up":-0.1}})", error);
	CHECK(!upLow.has_value());
	auto upHigh = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"up":5.1}})", error);
	CHECK(!upHigh.has_value());
}

TEST_CASE("ParseReach head pos boundaries")
{
	std::string error;
	auto        maxOk = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"lookAt":"hands","pos":[10,-10,10]}})", error);
	CHECK(maxOk.has_value());
	auto tooBig = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"pos":[10.1,0,0]}})", error);
	CHECK(!tooBig.has_value());
}

TEST_CASE("ParseReach head options need lookAt")
{
	std::string error;
	for (const char* key : { R"("back":1)", R"("up":1)", R"("pos":[1,2,3])", R"("eyeHeight":1.6)" }) {
		auto bare = Parse(std::string(R"({"hands":{"left":{"node":"A"}},"head":{)") + key + "}}", error);
		CHECK(!bare.has_value());
		CHECK(error.find("lookAt") != std::string::npos);
	}
	auto nulls = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"pos":null,"eyeHeight":null}})", error);
	CHECK(nulls.has_value());
}

TEST_CASE("ParseReach head eyeHeight")
{
	std::string error;
	auto        ok = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"lookAt":"hands","eyeHeight":1.6}})", error);
	CHECK(ok.has_value());
	if (ok) {
		CHECK(ok->head.eyeHeight.has_value());
		CHECK(Near(*ok->head.eyeHeight, 1.6));
	}
	auto none = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"lookAt":"hands"}})", error);
	CHECK(none.has_value() && !none->head.eyeHeight.has_value());
	auto low = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"eyeHeight":-0.1}})", error);
	CHECK(!low.has_value());
	auto high = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"eyeHeight":3.1}})", error);
	CHECK(!high.has_value());
	auto wrongType = Parse(R"({"hands":{"left":{"node":"A"}},"head":{"eyeHeight":"tall"}})", error);
	CHECK(!wrongType.has_value());
	CHECK(error.find("head.eyeHeight") != std::string::npos);
}

TEST_CASE("ParseReach holdMs boundaries")
{
	std::string error;
	auto        minOk = Parse(R"({"hands":{"left":{"node":"A"}},"holdMs":100})", error);
	CHECK(minOk.has_value());
	auto maxOk = Parse(R"({"hands":{"left":{"node":"A"}},"holdMs":1798000})", error);
	CHECK(maxOk.has_value());
	auto below = Parse(R"({"hands":{"left":{"node":"A"}},"holdMs":99})", error);
	CHECK(!below.has_value());
	auto above = Parse(R"({"hands":{"left":{"node":"A"}},"holdMs":1798001})", error);
	CHECK(!above.has_value());
}

TEST_CASE("ParseReach iterations boundaries")
{
	std::string error;
	auto        minOk = Parse(R"({"hands":{"left":{"node":"A"}},"iterations":1})", error);
	CHECK(minOk.has_value());
	auto maxOk = Parse(R"({"hands":{"left":{"node":"A"}},"iterations":10})", error);
	CHECK(maxOk.has_value());
	auto below = Parse(R"({"hands":{"left":{"node":"A"}},"iterations":0})", error);
	CHECK(!below.has_value());
	auto above = Parse(R"({"hands":{"left":{"node":"A"}},"iterations":11})", error);
	CHECK(!above.has_value());
}

TEST_CASE("ParseReach toleranceUnits boundaries")
{
	std::string error;
	auto        minOk = Parse(R"({"hands":{"left":{"node":"A"}},"toleranceUnits":0.05})", error);
	CHECK(minOk.has_value());
	auto maxOk = Parse(R"({"hands":{"left":{"node":"A"}},"toleranceUnits":50})", error);
	CHECK(maxOk.has_value());
	auto below = Parse(R"({"hands":{"left":{"node":"A"}},"toleranceUnits":0.04})", error);
	CHECK(!below.has_value());
	auto above = Parse(R"({"hands":{"left":{"node":"A"}},"toleranceUnits":50.1})", error);
	CHECK(!above.has_value());
}

TEST_CASE("ParseReach approachUnits boundaries")
{
	std::string error;
	auto        minOk = Parse(R"({"hands":{"left":{"node":"A"}},"approachUnits":0})", error);
	CHECK(minOk.has_value());
	auto maxOk = Parse(R"({"hands":{"left":{"node":"A"}},"approachUnits":200})", error);
	CHECK(maxOk.has_value());
	auto below = Parse(R"({"hands":{"left":{"node":"A"}},"approachUnits":-0.1})", error);
	CHECK(!below.has_value());
	auto above = Parse(R"({"hands":{"left":{"node":"A"}},"approachUnits":200.1})", error);
	CHECK(!above.has_value());
}

TEST_CASE("ParseReach node length limits")
{
	std::string error;
	std::string longName(128, 'a');
	std::string tooLong(129, 'a');
	std::string okJson = "{\"hands\":{\"left\":{\"node\":\"" + longName + "\"}}}";
	auto        ok = Parse(okJson, error);
	CHECK(ok.has_value());
	std::string badJson = "{\"hands\":{\"left\":{\"node\":\"" + tooLong + "\"}}}";
	auto        bad = Parse(badJson, error);
	CHECK(!bad.has_value());
}

TEST_CASE("MakeFrame builds the tracked set shape")
{
	Pose hmd = Upright(Vec3{ 0.0, 0.0, 0.0 });
	Pose left = Upright(Vec3{ 1.0, 0.0, 0.0 });
	Pose right = Upright(Vec3{ 2.0, 0.0, 0.0 });
	json frame = MakeFrame(0, 1, hmd, left, right);
	CHECK(frame["originCode"] == 1);
	CHECK(frame["tMs"] == 0);
	CHECK(frame["seq"] == 1);
	CHECK(frame["hmd"]["index"] == 0);
	CHECK(frame["hmd"]["available"] == true);
	CHECK(frame["hmd"]["trackingResult"] == 200);
	CHECK(frame["left"]["index"] == 1);
	CHECK(frame["right"]["index"] == 2);
	CHECK(frame["left"]["controller"]["packetNumber"] == 1);
	CHECK(frame["left"]["controller"]["pressed"] == 0);
	CHECK(frame["left"]["controller"]["axes"].size() == 5);
	CHECK(frame["left"]["matrix"].size() == 12);
	CHECK(frame["left"]["matrix"][3] == 1.0);
	CHECK(frame["right"]["matrix"][3] == 2.0);
}

TEST_CASE("CalibrationFrames produces four legs of four frames")
{
	Pose hmd = Upright(Vec3{ 0.0, 0.0, 0.0 });
	Vec3 rest{ 0.0, 0.0, 0.0 };
	Vec3 other{ 5.0, 0.0, 0.0 };
	json frames = CalibrationFrames(true, hmd, rest, other, 0.2);
	CHECK(frames.size() == 16);
	CHECK(frames[0]["tMs"] == 0);
	CHECK(frames[0]["seq"] == 1);
	CHECK(frames[15]["seq"] == 16);
	long previous = -1;
	for (const json& frame : frames) {
		long tMs = frame["tMs"].get<long>();
		CHECK(tMs > previous);
		previous = tMs;
	}
	Vec3 leg0 = PoseTranslation(frames[0], "right");
	Vec3 leg1 = PoseTranslation(frames[4], "right");
	Vec3 leg2 = PoseTranslation(frames[8], "right");
	Vec3 leg3 = PoseTranslation(frames[12], "right");
	CHECK(NearXYZ(leg0, 0.0, 0.0, 0.0));
	CHECK(NearXYZ(leg1, 0.2, 0.0, 0.0));
	CHECK(NearXYZ(leg2, 0.0, 0.2, 0.0));
	CHECK(NearXYZ(leg3, 0.0, 0.0, 0.2));
	for (const json& frame : frames) {
		Vec3 left = PoseTranslation(frame, "left");
		CHECK(NearXYZ(left, 5.0, 0.0, 0.0));
	}
}

TEST_CASE("CalibrationFrames moves the left hand when requested")
{
	Pose hmd = Upright(Vec3{ 0.0, 0.0, 0.0 });
	Vec3 rest{ 0.0, 0.0, 0.0 };
	Vec3 other{ 5.0, 0.0, 0.0 };
	json frames = CalibrationFrames(false, hmd, rest, other, 0.2);
	CHECK(frames.size() == 16);
	Vec3 leg1Left = PoseTranslation(frames[4], "left");
	CHECK(NearXYZ(leg1Left, 0.2, 0.0, 0.0));
	for (const json& frame : frames) {
		Vec3 right = PoseTranslation(frame, "right");
		CHECK(NearXYZ(right, 5.0, 0.0, 0.0));
	}
}

TEST_CASE("CalibrationSampleMs lands inside each leg")
{
	CHECK(CalibrationSampleMs(0) == 2600);
	CHECK(CalibrationSampleMs(1) == 6600);
	CHECK(CalibrationSampleMs(2) == 10600);
	CHECK(CalibrationSampleMs(3) == 14600);
}

TEST_CASE("ApproachAndHoldFrames blends then holds")
{
	Pose hmd = Upright(Vec3{ 0.0, 0.0, 0.0 });
	Vec3 leftStart{ 0.0, 0.0, 0.0 };
	Vec3 leftFinal{ 10.0, 0.0, 0.0 };
	Vec3 rightStart{ 0.0, 0.0, 0.0 };
	Vec3 rightFinal{ 0.0, 10.0, 0.0 };
	json frames = ApproachAndHoldFrames(hmd, leftStart, leftFinal, rightStart, rightFinal, 5000);
	CHECK(frames.size() == 11);
	CHECK(frames[0]["tMs"] == 0);
	CHECK(frames[5]["tMs"] == 1500);
	CHECK(frames[6]["tMs"] == 2500);
	long previous = -1;
	for (const json& frame : frames) {
		long tMs = frame["tMs"].get<long>();
		CHECK(tMs > previous);
		previous = tMs;
	}
	Vec3 firstLeft = PoseTranslation(frames[0], "left");
	Vec3 midLeft = PoseTranslation(frames[3], "left");
	Vec3 lastBlendLeft = PoseTranslation(frames[5], "left");
	Vec3 holdLeft = PoseTranslation(frames[6], "left");
	CHECK(NearXYZ(firstLeft, 0.0, 0.0, 0.0));
	CHECK(NearXYZ(midLeft, 6.0, 0.0, 0.0));
	CHECK(NearXYZ(lastBlendLeft, 10.0, 0.0, 0.0));
	CHECK(NearXYZ(holdLeft, 10.0, 0.0, 0.0));
	Vec3 firstRight = PoseTranslation(frames[0], "right");
	Vec3 lastBlendRight = PoseTranslation(frames[5], "right");
	CHECK(NearXYZ(firstRight, 0.0, 0.0, 0.0));
	CHECK(NearXYZ(lastBlendRight, 0.0, 10.0, 0.0));
}

TEST_CASE("PhaseName returns lower-camel names")
{
	CHECK(PhaseName(Phase::kQueued) == "queued");
	CHECK(PhaseName(Phase::kCalibrating) == "calibrating");
	CHECK(PhaseName(Phase::kReaching) == "reaching");
	CHECK(PhaseName(Phase::kHolding) == "holding");
	CHECK(PhaseName(Phase::kDone) == "done");
	CHECK(PhaseName(Phase::kFailed) == "failed");
	CHECK(PhaseName(Phase::kStopped) == "stopped");
}

TEST_CASE("ProgressJson rounds and shapes per-hand data")
{
	Progress progress;
	progress.phase = Phase::kReaching;
	progress.iteration = 2;
	HandProgress hand;
	hand.hand = Vec3{ 0.0, 0.0, 0.0 };
	hand.node = Vec3{ 3.0, 4.0, 0.0 };
	hand.error = Vec3{ 3.0, 4.0, 0.0 };
	progress.left = hand;
	json result = ProgressJson(progress);
	CHECK(result["phase"] == "reaching");
	CHECK(result["iteration"] == 2);
	CHECK(result["left"]["hand"].size() == 3);
	CHECK(result["left"]["hand"][0] == 0.0);
	CHECK(result["left"]["errorDistance"] == 5.0);
	CHECK(result["left"]["handToNode"] == 5.0);
	CHECK(result["worstErrorDistance"] == 5.0);
	CHECK(!result.contains("right"));
	CHECK(!result.contains("error"));
}

TEST_CASE("ProgressJson reports the larger of the two hands' errors")
{
	Progress     progress;
	HandProgress near;
	near.error = Vec3{ 1.0, 0.0, 0.0 };
	HandProgress far;
	far.error = Vec3{ 0.0, 6.0, 8.0 };
	progress.left = near;
	progress.right = far;
	json result = ProgressJson(progress);
	CHECK(result["worstErrorDistance"] == 10.0);

	Progress empty;
	CHECK(!ProgressJson(empty).contains("worstErrorDistance"));
}

TEST_CASE("ProgressJson rounds to hundredths")
{
	Progress     progress;
	HandProgress hand;
	hand.hand = Vec3{ 1.234, 5.678, 9.012 };
	hand.node = Vec3{ 0.0, 0.0, 0.0 };
	hand.error = Vec3{ 0.0, 0.0, 0.0 };
	progress.right = hand;
	json result = ProgressJson(progress);
	CHECK(result["right"]["hand"][0] == 1.23);
	CHECK(result["right"]["hand"][1] == 5.68);
	CHECK(result["right"]["hand"][2] == 9.01);
}

TEST_CASE("ProgressJson includes the error string when set")
{
	Progress progress;
	progress.phase = Phase::kFailed;
	progress.error = "boom";
	json result = ProgressJson(progress);
	CHECK(result["phase"] == "failed");
	CHECK(result["error"] == "boom");
}

TEST_CASE("Converged compares worst error to tolerance")
{
	CHECK(Converged(0.5, 1.0));
	CHECK(Converged(1.0, 1.0));
	CHECK(!Converged(1.1, 1.0));
	CHECK(Converged(0.0, 0.05));
}

TEST_CASE("ApproachAndHoldDurationMs is the last frame time")
{
	Pose hmd = Upright(Vec3{ 0.0, 0.0, 0.0 });
	Vec3 origin{ 0.0, 0.0, 0.0 };
	for (long hold : { 100L, 999L, 1000L, 5000L, 12500L }) {
		json frames = ApproachAndHoldFrames(hmd, origin, origin, origin, origin, hold);
		CHECK(frames.back()["tMs"].get<long>() == ApproachAndHoldDurationMs(hold));
	}
}
