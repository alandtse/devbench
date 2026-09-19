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

	// Keyframes of a recorded scenario on its own clock: a pose step sits at its `atMs`, or at the
	// sum of preceding waits for recordings that predate `atMs`. Strictly increasing in time.
	std::vector<PoseKeyframe> ExtractKeyframes(const json& a_steps);

	// The player's pose as a pure function of absolute elapsed time, so playback is identical at
	// any frame rate. Times outside the recording clamp to its first/last keyframe.
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

	// Signed shortest rotation from a_fromDeg to a_toDeg, in (-180, 180].
	double ShortestArcDeltaDeg(double a_fromDeg, double a_toDeg);
}
