#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Json.h"

namespace dvb::ReachLogic
{

	/// 3D vector used for both tracking meters and world units.
	struct Vec3
	{
		double x{ 0 }, y{ 0 }, z{ 0 };
	};

	Vec3   Add(const Vec3& a_a, const Vec3& a_b);
	Vec3   Sub(const Vec3& a_a, const Vec3& a_b);
	Vec3   Scale(const Vec3& a_v, double a_s);
	Vec3   Cross(const Vec3& a_a, const Vec3& a_b);
	double Dot(const Vec3& a_a, const Vec3& a_b);
	double Norm(const Vec3& a_v);
	/// Returns the zero vector when the input length is ~zero.
	Vec3   Unit(const Vec3& a_v);
	double Distance(const Vec3& a_a, const Vec3& a_b);

	/// 3x4 row-major OpenVR pose; columns are right, up, back; translation in 3, 7, 11.
	using Pose = std::array<double, 12>;

	Pose Upright(const Vec3& a_position);
	/// Device forward points at the target; degenerate inputs fall back to a finite pose.
	Pose LookAt(const Vec3& a_position, const Vec3& a_target);

	/// Linear map from tracking meters to world units, measured at a rest pose.
	struct Calibration
	{
		Vec3                rest;
		std::array<Vec3, 3> perMeter;
	};

	/// points[0] is the node at rest, points[i+1] after moving +step along tracking axis i.
	std::optional<Calibration> CalibrationFromPoints(const std::array<Vec3, 4>& a_points, double a_stepMeters);
	/// Solves J d = delta by Cramer's rule; nullopt when J is singular.
	std::optional<Vec3> TrackingDelta(const Calibration& a_calibration, const Vec3& a_worldDelta);
	/// restTracking + J^-1 (world - calibration.rest); falls back to restTracking if singular.
	Vec3 TrackingFor(const Calibration& a_calibration, const Vec3& a_restTracking, const Vec3& a_world);

	struct HandTarget
	{
		std::string node;
		double      front{ 0 };
		Vec3        offset;
	};

	struct HeadTarget
	{
		enum class Aim
		{
			kNone,
			kHands,
			kNode
		};
		Aim                   aim{ Aim::kNone };
		std::string           node;
		double                back{ 0.5 };
		double                up{ 0.3 };
		std::optional<Vec3>   position;
		std::optional<double> eyeHeight;
	};

	struct ReachRequest
	{
		std::optional<HandTarget> left;
		std::optional<HandTarget> right;
		HeadTarget                head;
		long                      holdMs{ 10000 };
		int                       iterations{ 4 };
		double                    toleranceUnits{ 1.0 };
		double                    approachUnits{ 15.0 };
	};

	/// Parses the `reach` tool arguments; on failure fills a_error and returns nullopt.
	std::optional<ReachRequest> ParseReach(const json& a_args, std::string& a_error);

	inline constexpr double      kFrontMin = -500.0;
	inline constexpr double      kFrontMax = 500.0;
	inline constexpr double      kOffsetLimit = 1000.0;
	inline constexpr double      kBackMin = 0.0;
	inline constexpr double      kBackMax = 5.0;
	inline constexpr double      kUpMin = 0.0;
	inline constexpr double      kUpMax = 5.0;
	inline constexpr double      kPosLimit = 10.0;
	inline constexpr double      kEyeHeightMax = 3.0;
	inline constexpr long        kHoldMsMin = 100;
	inline constexpr long        kHoldMsMax = 1798000;
	inline constexpr int         kIterationsMin = 1;
	inline constexpr int         kIterationsMax = 10;
	inline constexpr double      kToleranceMin = 0.05;
	inline constexpr double      kToleranceMax = 50.0;
	inline constexpr double      kApproachMin = 0.0;
	inline constexpr double      kApproachMax = 200.0;
	inline constexpr std::size_t kNodeMaxLen = 128;
	inline constexpr long        kCalibrationLegMs = 4000;
	inline constexpr long        kCalibrationTotalMs = 16000;

	/// Builds one vrTrackedSet frame; seq mirrors the 1-based packet number.
	json MakeFrame(long a_tMs, int a_packet, const Pose& a_hmd, const Pose& a_left, const Pose& a_right);
	/// Four legs of four frames; the calibrated hand moves +step along x, y, z.
	json CalibrationFrames(bool a_rightHand, const Pose& a_hmd, const Vec3& a_restTracking, const Vec3& a_otherHandTracking, double a_stepMeters);
	/// When to read the node after a leg's pose has settled.
	long CalibrationSampleMs(int a_leg);
	/// Milliseconds from the first frame to the last of ApproachAndHoldFrames for the same hold.
	long ApproachAndHoldDurationMs(long a_holdMs);
	/// Six blend frames then hold frames at the final pose.
	json ApproachAndHoldFrames(const Pose& a_hmd, const Vec3& a_leftStart, const Vec3& a_leftFinal, const Vec3& a_rightStart, const Vec3& a_rightFinal, long a_holdMs);

	enum class Phase
	{
		kQueued,
		kCalibrating,
		kReaching,
		kHolding,
		kDone,
		kFailed,
		kStopped
	};
	std::string_view PhaseName(Phase a_phase);

	struct HandProgress
	{
		Vec3 hand;
		Vec3 node;
		Vec3 error;
	};

	struct Progress
	{
		Phase                       phase{ Phase::kQueued };
		int                         iteration{ 0 };
		std::optional<HandProgress> left;
		std::optional<HandProgress> right;
		std::string                 error;
	};

	json ProgressJson(const Progress& a_progress);
	bool Converged(double a_worstErrorUnits, double a_toleranceUnits);

}
