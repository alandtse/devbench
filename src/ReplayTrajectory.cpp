#include "ReplayTrajectory.h"

#include <algorithm>
#include <cmath>

namespace dvb::Recording
{
	namespace
	{
		constexpr double kFullTurnDeg = 360.0;
		constexpr double kHalfTurnDeg = 180.0;
		constexpr double kPi = 3.14159265358979323846;
		constexpr double kTwoPi = 2.0 * kPi;

		double NormalizeDeg(double a_deg)
		{
			double d = std::fmod(a_deg, kFullTurnDeg);
			return d < 0.0 ? d + kFullTurnDeg : d;
		}

		double Distance(const Pose& a_a, const Pose& a_b)
		{
			return std::sqrt((a_a.x - a_b.x) * (a_a.x - a_b.x) + (a_a.y - a_b.y) * (a_a.y - a_b.y) +
							 (a_a.z - a_b.z) * (a_a.z - a_b.z));
		}

		bool ReadPose(const json& a_pose, Pose& a_out)
		{
			if (!a_pose.is_array() || a_pose.size() < 5)
				return false;
			for (std::size_t i = 0; i < 5; ++i)
				if (!a_pose[i].is_number())
					return false;
			a_out = { a_pose[0].get<double>(), a_pose[1].get<double>(), a_pose[2].get<double>(),
				a_pose[3].get<double>(), a_pose[4].get<double>() };
			return true;
		}

		// [camX, camY, camZ, camPitchRad, camYawRad] -- the recording's own captured camera
		// transform for this sample, when present (see BuildScenario).
		void ReadCamPose(const json& a_camPose, Pose& a_out)
		{
			if (!a_camPose.is_array() || a_camPose.size() < 5)
				return;
			for (std::size_t i = 0; i < 5; ++i)
				if (!a_camPose[i].is_number())
					return;
			a_out.camX = a_camPose[0].get<double>();
			a_out.camY = a_camPose[1].get<double>();
			a_out.camZ = a_camPose[2].get<double>();
			a_out.camPitch = a_camPose[3].get<double>();
			a_out.camYaw = a_camPose[4].get<double>();
		}
	}

	bool IsValidPose(const json& a_pose)
	{
		Pose pose;
		return ReadPose(a_pose, pose);
	}

	double ShortestArcDeltaDeg(double a_fromDeg, double a_toDeg)
	{
		double delta = std::fmod(a_toDeg - a_fromDeg, kFullTurnDeg);
		if (delta > kHalfTurnDeg)
			delta -= kFullTurnDeg;
		else if (delta <= -kHalfTurnDeg)
			delta += kFullTurnDeg;
		return delta;
	}

	std::vector<PoseKeyframe> ExtractKeyframes(const json& a_steps)
	{
		std::vector<PoseKeyframe> keyframes;
		if (!a_steps.is_array())
			return keyframes;
		std::int64_t clockMs = 0;
		// BuildScenario only emits camPose on a row whose camera value actually changed; an
		// unchanged sample carries none. Reuse the last-seen camera fields here so that
		// compaction doesn't look like a gap in the recorded transform.
		Pose lastCamPose;
		bool haveCamPose = false;
		for (const auto& step : a_steps) {
			if (!step.is_object())
				continue;
			if (step.contains("atMs") && step["atMs"].is_number())
				clockMs = step["atMs"].get<std::int64_t>();
			Pose pose;
			if (step.contains("pose") && ReadPose(step["pose"], pose)) {
				if (step.contains("camPose")) {
					ReadCamPose(step["camPose"], pose);
					lastCamPose = pose;
					haveCamPose = true;
				} else if (haveCamPose) {
					pose.camX = lastCamPose.camX;
					pose.camY = lastCamPose.camY;
					pose.camZ = lastCamPose.camZ;
					pose.camPitch = lastCamPose.camPitch;
					pose.camYaw = lastCamPose.camYaw;
				}
				if (!keyframes.empty() && keyframes.back().tMs == clockMs)
					keyframes.back().pose = pose;
				else
					keyframes.push_back({ clockMs, pose });
			}
			if (step.contains("wait") && step["wait"].is_number())
				clockMs += std::max<std::int64_t>(0, step["wait"].get<std::int64_t>());
		}
		return keyframes;
	}

	json ScaleWaitsToRecordedDuration(const json& a_steps, std::int64_t a_recordedMs)
	{
		if (!a_steps.is_array() || a_recordedMs <= 0)
			return a_steps;
		std::int64_t totalWaitMs = 0;
		for (const auto& step : a_steps) {
			if (step.is_object() && step.contains("atMs"))
				return a_steps;
			if (step.is_object() && step.contains("wait") && step["wait"].is_number())
				totalWaitMs += std::max<std::int64_t>(0, step["wait"].get<std::int64_t>());
		}
		if (totalWaitMs <= 0 || a_recordedMs <= totalWaitMs)
			return a_steps;

		const double factor = static_cast<double>(a_recordedMs) / static_cast<double>(totalWaitMs);
		json         scaled = a_steps;
		double       exact = 0.0;
		std::int64_t emitted = 0;
		for (auto& step : scaled) {
			if (!step.is_object() || !step.contains("wait") || !step["wait"].is_number())
				continue;
			// Cumulative rounding keeps the scaled clock within half a millisecond of the target.
			exact += static_cast<double>(std::max<std::int64_t>(0, step["wait"].get<std::int64_t>())) * factor;
			const auto target = static_cast<std::int64_t>(std::llround(exact));
			step["wait"] = target - emitted;
			emitted = target;
		}
		return scaled;
	}

	Trajectory::Trajectory(std::vector<PoseKeyframe> a_keyframes, Interpolation a_mode) :
		m_keyframes(std::move(a_keyframes)), m_mode(a_mode)
	{
		std::stable_sort(m_keyframes.begin(), m_keyframes.end(),
			[](const PoseKeyframe& a, const PoseKeyframe& b) { return a.tMs < b.tMs; });
		std::vector<PoseKeyframe> unique;
		for (const auto& key : m_keyframes) {
			if (!unique.empty() && unique.back().tMs == key.tMs)
				unique.back() = key;
			else
				unique.push_back(key);
		}
		m_keyframes = std::move(unique);
	}

	bool Trajectory::IsTeleport(std::size_t a_index) const
	{
		return Distance(m_keyframes[a_index].pose, m_keyframes[a_index + 1].pose) > kTeleportDistanceUnits;
	}

	// Finite-difference tangent (units per ms) that ignores neighbours across a teleport so the
	// spline never overshoots into or out of one.
	double Trajectory::Tangent(std::size_t a_index, double Pose::* a_axis) const
	{
		const bool hasPrev = a_index > 0 && !IsTeleport(a_index - 1);
		const bool hasNext = a_index + 1 < m_keyframes.size() && !IsTeleport(a_index);
		const auto value = [&](std::size_t i) { return m_keyframes[i].pose.*a_axis; };
		const auto time = [&](std::size_t i) { return static_cast<double>(m_keyframes[i].tMs); };
		if (hasPrev && hasNext)
			return (value(a_index + 1) - value(a_index - 1)) / (time(a_index + 1) - time(a_index - 1));
		if (hasNext)
			return (value(a_index + 1) - value(a_index)) / (time(a_index + 1) - time(a_index));
		if (hasPrev)
			return (value(a_index) - value(a_index - 1)) / (time(a_index) - time(a_index - 1));
		return 0.0;
	}

	Pose Trajectory::Sample(double a_tMs) const
	{
		if (m_keyframes.empty())
			return {};
		if (a_tMs <= static_cast<double>(m_keyframes.front().tMs))
			return m_keyframes.front().pose;
		if (a_tMs >= static_cast<double>(m_keyframes.back().tMs))
			return m_keyframes.back().pose;

		const auto        upper = std::upper_bound(m_keyframes.begin(), m_keyframes.end(), a_tMs,
			[](double a_t, const PoseKeyframe& a_key) { return a_t < static_cast<double>(a_key.tMs); });
		const std::size_t next = static_cast<std::size_t>(upper - m_keyframes.begin());
		const std::size_t prev = next - 1;
		const Pose&       a = m_keyframes[prev].pose;
		const Pose&       b = m_keyframes[next].pose;
		if (IsTeleport(prev))
			return a;

		const double t0 = static_cast<double>(m_keyframes[prev].tMs);
		const double dt = static_cast<double>(m_keyframes[next].tMs) - t0;
		const double u = (a_tMs - t0) / dt;

		Pose       out;
		const auto blend = [&](double Pose::* a_axis) {
			const double p0 = a.*a_axis;
			const double p1 = b.*a_axis;
			if (m_mode == Interpolation::Linear)
				return p0 + (p1 - p0) * u;
			const double u2 = u * u;
			const double u3 = u2 * u;
			return (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * dt * Tangent(prev, a_axis) +
			       (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * dt * Tangent(next, a_axis);
		};
		out.x = blend(&Pose::x);
		out.y = blend(&Pose::y);
		out.z = blend(&Pose::z);
		out.yawDeg = NormalizeDeg(a.yawDeg + ShortestArcDeltaDeg(a.yawDeg, b.yawDeg) * u);
		out.pitchDeg = a.pitchDeg + (b.pitchDeg - a.pitchDeg) * u;

		// Only when BOTH neighbours have camera data; a lone gap falls back at replay time.
		if (a.HasCam() && b.HasCam()) {
			out.camX = *a.camX + (*b.camX - *a.camX) * u;
			out.camY = *a.camY + (*b.camY - *a.camY) * u;
			out.camZ = *a.camZ + (*b.camZ - *a.camZ) * u;
			out.camPitch = *a.camPitch + (*b.camPitch - *a.camPitch) * u;
			double yawDelta = std::fmod(*b.camYaw - *a.camYaw, kTwoPi);
			if (yawDelta > kPi)
				yawDelta -= kTwoPi;
			else if (yawDelta < -kPi)
				yawDelta += kTwoPi;
			out.camYaw = *a.camYaw + yawDelta * u;
		}
		return out;
	}
}
