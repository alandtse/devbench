#pragma once

#include <optional>
#include <string_view>

namespace dvb::CameraFrame
{
	enum class Side
	{
		kFront,
		kBack,
		kLeft,
		kRight,
		kTop,
		kBottom,
	};

	/// front | back | left | right | top | bottom (case-sensitive); nullopt for anything else.
	std::optional<Side> ParseSide(std::string_view a_name);

	/// A reference's placement: world position, heading in radians (inspect refs rotation[2]) and
	/// the local Z extents of its bounds. A zero-height bound means "unknown".
	struct Target
	{
		double x = 0;
		double y = 0;
		double z = 0;
		double heading = 0;
		double boundMinZ = 0;
		double boundMaxZ = 0;
	};

	struct Options
	{
		Side                  side = Side::kFront;
		std::optional<double> aroundRad;  ///< clockwise from the target's front; replaces the side's horizontal angle
		std::optional<double> distance;   ///< world units; default kDistanceFactor x height x distanceScale
		double                distanceScale = 1.0;
		double                eyeFraction = 0.85;  ///< camera height as a fraction of the target's height
		std::optional<double> pitchRad;            ///< positive looks down; default 0 (top and bottom use +-kMaxPitch)
	};

	/// Native free-camera pose, as `camera action=drive` takes it.
	struct Pose
	{
		double x = 0;
		double y = 0;
		double z = 0;
		double pitch = 0;
		double yaw = 0;
	};

	inline constexpr double kDefaultHeight = 128.0;          // a standing human; used when the bounds are empty
	inline constexpr double kDistanceFactor = 1.4;           // x height: head and shoulders with the face readable
	inline constexpr double kMaxPitch = 1.5533430342749532;  // 89 degrees

	/// Place the camera on the chosen side of `a_target`, looking at it. Yaw increases clockwise from
	/// +Y and the view direction is (sin yaw, cos yaw), the same sense as a reference's heading, so a
	/// camera at heading + offset looks back at the target with yaw = heading + offset + pi.
	Pose Frame(const Target& a_target, const Options& a_options);

	/// Wrap an angle into (-pi, pi].
	double WrapPi(double a_angle);
}
