// Pose math for camera action=frame.

#include "test_framework.h"

#include <cmath>
#include <limits>
#include <numbers>
#include <string>

#include "CameraFrameLogic.h"

using namespace dvb::CameraFrame;

namespace
{
	Target Balgruuf()
	{
		Target t;
		t.x = 875.1173095703125;
		t.y = 2009.2069091796875;
		t.z = -80.0;
		t.heading = 1.6715143918991089;
		t.boundMinZ = 0.0;
		t.boundMaxZ = 128.0;
		return t;
	}

	Options At180(Side a_side)
	{
		Options o;
		o.side = a_side;
		o.distance = 180.0;
		return o;
	}

	bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
}

TEST_CASE("front is one distance along the heading, looking back at the target")
{
	const Pose p = Frame(Balgruuf(), At180(Side::kFront));
	CHECK(Near(p.x, 1054.27, 0.5));
	CHECK(Near(p.y, 1991.14, 0.5));
	CHECK(Near(p.z, 28.8, 1e-6));
	CHECK(Near(p.pitch, 0.0, 1e-9));
	CHECK(Near(p.yaw, -1.4701, 1e-3));
}

TEST_CASE("right is the target's clockwise side and shows his profile")
{
	const Pose p = Frame(Balgruuf(), At180(Side::kRight));
	CHECK(Near(p.x, 857.05, 0.5));
	CHECK(Near(p.y, 1830.1, 0.5));
	CHECK(Near(p.yaw, 0.1007, 1e-3));
}

TEST_CASE("back and left complete the circle")
{
	const Pose back = Frame(Balgruuf(), At180(Side::kBack));
	CHECK(Near(back.x, 695.97, 0.5));
	CHECK(Near(back.y, 2027.3, 0.5));
	CHECK(Near(back.yaw, 1.6715, 1e-3));

	const Pose left = Frame(Balgruuf(), At180(Side::kLeft));
	CHECK(Near(left.x, 893.19, 0.5));
	CHECK(Near(left.y, 2188.3, 0.5));
	CHECK(Near(left.yaw, -3.0409, 1e-3));
}

TEST_CASE("every horizontal side looks at the target")
{
	for (Side side : { Side::kFront, Side::kBack, Side::kLeft, Side::kRight }) {
		const Target t = Balgruuf();
		const Pose   p = Frame(t, At180(side));
		const double dx = t.x - p.x, dy = t.y - p.y;
		CHECK(Near(std::sin(p.yaw), dx / std::hypot(dx, dy), 1e-6));
		CHECK(Near(std::cos(p.yaw), dy / std::hypot(dx, dy), 1e-6));
		CHECK(Near(std::hypot(dx, dy), 180.0, 1e-6));
	}
}

TEST_CASE("default distance scales with the target's height")
{
	const Target t = Balgruuf();
	Options      o;
	const Pose   p = Frame(t, o);
	CHECK(Near(std::hypot(p.x - t.x, p.y - t.y), kDistanceFactor * 128.0, 1e-6));

	o.distanceScale = 2.0;
	CHECK(Near(std::hypot(Frame(t, o).x - t.x, Frame(t, o).y - t.y), 2.0 * kDistanceFactor * 128.0, 1e-6));
}

TEST_CASE("empty bounds fall back to a standing human's height")
{
	Target t;  // no bounds
	t.heading = 0.0;
	const Pose p = Frame(t, Options{});
	CHECK(Near(p.z, 0.85 * kDefaultHeight, 1e-6));
	CHECK(Near(p.y, kDistanceFactor * kDefaultHeight, 1e-6));  // heading 0 faces +Y, so front is north of the target
	CHECK(Near(p.x, 0.0, 1e-6));
}

TEST_CASE("an explicit around angle replaces the side's horizontal angle")
{
	Options o = At180(Side::kFront);
	o.aroundRad = std::numbers::pi / 4.0;
	Target t = Balgruuf();
	t.heading = 0.0;
	const Pose p = Frame(t, o);
	CHECK(Near(p.x - t.x, 180.0 * std::sin(std::numbers::pi / 4.0), 1e-6));
	CHECK(Near(p.y - t.y, 180.0 * std::cos(std::numbers::pi / 4.0), 1e-6));
}

TEST_CASE("top looks down from above the target, bottom looks up from below")
{
	const Target t = Balgruuf();
	const Pose   top = Frame(t, At180(Side::kTop));
	CHECK(Near(top.x, t.x, 1e-9));
	CHECK(Near(top.y, t.y, 1e-9));
	CHECK(Near(top.z, t.z + 64.0 + 180.0, 1e-6));
	CHECK(Near(top.pitch, kMaxPitch, 1e-9));
	CHECK(Near(top.yaw, t.heading, 1e-9));

	const Pose bottom = Frame(t, At180(Side::kBottom));
	CHECK(Near(bottom.z, t.z + 64.0 - 180.0, 1e-6));
	CHECK(Near(bottom.pitch, -kMaxPitch, 1e-9));
}

TEST_CASE("an explicit pitch overrides the default")
{
	Options o = At180(Side::kFront);
	o.pitchRad = 0.2;
	CHECK(Near(Frame(Balgruuf(), o).pitch, 0.2, 1e-12));
}

TEST_CASE("side names parse exactly")
{
	CHECK(ParseSide("front") == Side::kFront);
	CHECK(ParseSide("back") == Side::kBack);
	CHECK(ParseSide("left") == Side::kLeft);
	CHECK(ParseSide("right") == Side::kRight);
	CHECK(ParseSide("top") == Side::kTop);
	CHECK(ParseSide("bottom") == Side::kBottom);
	CHECK(!ParseSide("Front").has_value());
	CHECK(!ParseSide("").has_value());
}

TEST_CASE("angles wrap into (-pi, pi]")
{
	constexpr double pi = std::numbers::pi;
	CHECK(Near(WrapPi(pi), pi, 1e-12));
	CHECK(Near(WrapPi(-pi), pi, 1e-12));
	CHECK(Near(WrapPi(3.0 * pi), pi, 1e-9));
	CHECK(Near(WrapPi(2.0 * pi + 0.1), 0.1, 1e-9));
	CHECK(Near(WrapPi(-2.0 * pi - 0.1), -0.1, 1e-9));
}

TEST_CASE("options within their limits are accepted")
{
	CHECK(!Validate(Options{}).has_value());
	Options o;
	o.distance = kMaxDistance;
	o.distanceScale = kMaxDistanceScale;
	o.eyeFraction = kMaxEyeFraction;
	o.pitchRad = kMaxPitch;
	o.aroundRad = 12.0;
	CHECK(!Validate(o).has_value());
}

TEST_CASE("options outside their limits are rejected with the parameter named")
{
	const auto reject = [](Options o, const char* a_name) {
		const auto why = Validate(o);
		return why.has_value() && why->find(a_name) != std::string::npos;
	};
	Options o;
	o.distance = 0.0;
	CHECK(reject(o, "'distance'"));
	o.distance = kMaxDistance * 2.0;
	CHECK(reject(o, "'distance'"));
	o = Options{};
	o.distanceScale = 0.0;
	CHECK(reject(o, "'distanceScale'"));
	o.distanceScale = kMaxDistanceScale + 1.0;
	CHECK(reject(o, "'distanceScale'"));
	o = Options{};
	o.eyeFraction = -0.1;
	CHECK(reject(o, "'eyeHeight'"));
	o.eyeFraction = kMaxEyeFraction + 0.1;
	CHECK(reject(o, "'eyeHeight'"));
	o = Options{};
	o.pitchRad = kMaxPitch + 0.01;
	CHECK(reject(o, "'pitchDeg'"));
	o.pitchRad = -kMaxPitch - 0.01;
	CHECK(reject(o, "'pitchDeg'"));
	o = Options{};
	o.aroundRad = std::numeric_limits<double>::infinity();
	CHECK(reject(o, "'aroundDeg'"));
	o.aroundRad = std::numeric_limits<double>::quiet_NaN();
	CHECK(reject(o, "'aroundDeg'"));
}
