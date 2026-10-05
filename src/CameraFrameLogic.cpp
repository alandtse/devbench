#include "CameraFrameLogic.h"

#include <cmath>
#include <format>
#include <numbers>

namespace dvb::CameraFrame
{
	namespace
	{
		constexpr double kPi = std::numbers::pi;
	}

	std::optional<Side> ParseSide(std::string_view a_name)
	{
		if (a_name == "front")
			return Side::kFront;
		if (a_name == "back")
			return Side::kBack;
		if (a_name == "left")
			return Side::kLeft;
		if (a_name == "right")
			return Side::kRight;
		if (a_name == "top")
			return Side::kTop;
		if (a_name == "bottom")
			return Side::kBottom;
		return std::nullopt;
	}

	std::optional<std::string> Validate(const Options& a_options)
	{
		if (a_options.aroundRad && !std::isfinite(*a_options.aroundRad))
			return "'aroundDeg' must be finite";
		if (a_options.distance && !(*a_options.distance > 0.0 && *a_options.distance <= kMaxDistance))
			return std::format("'distance' must be > 0 and <= {}", kMaxDistance);
		if (!(a_options.distanceScale > 0.0 && a_options.distanceScale <= kMaxDistanceScale))
			return std::format("'distanceScale' must be > 0 and <= {}", kMaxDistanceScale);
		if (!(a_options.eyeFraction >= 0.0 && a_options.eyeFraction <= kMaxEyeFraction))
			return std::format("'eyeHeight' must be within 0..{} (a fraction of the reference's height)", kMaxEyeFraction);
		if (a_options.pitchRad && !(std::fabs(*a_options.pitchRad) <= kMaxPitch))
			return std::format("'pitchDeg' must be within -{0}..{0}", kMaxPitchDeg);
		return std::nullopt;
	}

	double WrapPi(double a_angle)
	{
		double wrapped = std::fmod(a_angle, 2.0 * kPi);
		if (wrapped > kPi)
			wrapped -= 2.0 * kPi;
		else if (wrapped <= -kPi)
			wrapped += 2.0 * kPi;
		return wrapped;
	}

	Pose Frame(const Target& a_target, const Options& a_options)
	{
		double height = a_target.boundMaxZ - a_target.boundMinZ;
		if (!(height > 1.0))
			height = kDefaultHeight;
		const double distance = a_options.distance.value_or(kDistanceFactor * height * a_options.distanceScale);
		const double baseZ = a_target.z + a_target.boundMinZ;

		Pose pose;
		if (a_options.side == Side::kTop || a_options.side == Side::kBottom) {
			const bool top = a_options.side == Side::kTop;
			pose.x = a_target.x;
			pose.y = a_target.y;
			pose.z = baseZ + 0.5 * height + (top ? distance : -distance);
			pose.pitch = a_options.pitchRad.value_or(top ? kMaxPitch : -kMaxPitch);
			pose.yaw = WrapPi(a_target.heading + a_options.aroundRad.value_or(0.0));  // image up is the target's front
			return pose;
		}

		double offset = 0.0;
		switch (a_options.side) {
		case Side::kRight:
			offset = kPi / 2.0;
			break;
		case Side::kBack:
			offset = kPi;
			break;
		case Side::kLeft:
			offset = -kPi / 2.0;
			break;
		default:
			break;
		}
		const double around = a_target.heading + a_options.aroundRad.value_or(offset);
		pose.x = a_target.x + distance * std::sin(around);
		pose.y = a_target.y + distance * std::cos(around);
		pose.z = baseZ + a_options.eyeFraction * height;
		pose.pitch = a_options.pitchRad.value_or(0.0);
		pose.yaw = WrapPi(around + kPi);
		return pose;
	}
}
