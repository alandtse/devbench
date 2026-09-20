#pragma once

#include "Json.h"

#include <cstdint>
#include <vector>

namespace dvb::Recording
{
	struct Pose
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
		double yawDeg = 0.0;
		double pitchDeg = 0.0;
	};

	struct PoseKeyframe
	{
		std::int64_t tMs = 0;
		Pose         pose;
	};

	enum class Interpolation
	{
		Linear,
		CatmullRom,
	};

	// Distance between consecutive keyframes above which the gap is a teleport (cell load, coc)
	// rather than movement, so the trajectory holds and snaps instead of sliding across it.
	inline constexpr double kTeleportDistanceUnits = 2000.0;

	bool IsValidPose(const json& a_pose);

	// Keyframes of a recorded scenario on its own clock: a pose step sits at its `atMs`, or at the
	// sum of preceding waits for recordings that predate `atMs`. Strictly increasing in time.
	std::vector<PoseKeyframe> ExtractKeyframes(const json& a_steps);

	// The player's pose as a pure function of absolute elapsed time. Times outside the recording clamp
	// to its first or last keyframe.
	class Trajectory
	{
	public:
		explicit Trajectory(std::vector<PoseKeyframe> a_keyframes,
			Interpolation                             a_mode = Interpolation::Linear);

		[[nodiscard]] bool         Empty() const { return m_keyframes.empty(); }
		[[nodiscard]] std::int64_t StartMs() const { return m_keyframes.empty() ? 0 : m_keyframes.front().tMs; }
		[[nodiscard]] std::int64_t EndMs() const { return m_keyframes.empty() ? 0 : m_keyframes.back().tMs; }

		[[nodiscard]] Pose Sample(double a_tMs) const;

	private:
		[[nodiscard]] bool   IsTeleport(std::size_t a_index) const;
		[[nodiscard]] double Tangent(std::size_t a_index, double Pose::* a_axis) const;

		std::vector<PoseKeyframe> m_keyframes;
		Interpolation             m_mode;
	};

	// Scales the waits of recordings that predate `atMs` so their clock spans a_recordedMs.
	json ScaleWaitsToRecordedDuration(const json& a_steps, std::int64_t a_recordedMs);

	double ShortestArcDeltaDeg(double a_fromDeg, double a_toDeg);
}
