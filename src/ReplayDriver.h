#pragma once

#include "Json.h"
#include "ReplayTrajectory.h"

#include <chrono>
#include <memory>
#include <optional>

namespace dvb::Recording::ReplayDriver
{
	// Drives the player along a trajectory from the game's main thread, once per engine frame,
	// with pose sampled from absolute elapsed time. Destroying the session stops it.
	class Session
	{
	public:
		virtual ~Session() = default;

		[[nodiscard]] virtual json Stats() const = 0;

		// Blocks until the final pose has been applied. False if a_timeout passes first.
		virtual bool WaitFinished(std::chrono::milliseconds a_timeout) = 0;
	};

	// Begins driving immediately: elapsed time zero is the trajectory's first keyframe. Null when
	// the engine frame counter cannot be read, since the driver paces itself by it.
	std::unique_ptr<Session> Start(Trajectory a_trajectory);

	// One scenario run's use of the driver: starts it at the first pose step, keeps the scenario's
	// waits on the driver's absolute clock, and collects its stats when it ends.
	class Playback
	{
	public:
		// Throws ToolError(400) if a_enabled and any pose step is malformed. a_steps must outlive this.
		Playback(const json& a_steps, bool a_enabled);

		// True if the driver moved the player for this step, so the caller must not teleport.
		bool Handle(const json& a_step);

		// Sleeps a_ms of GAME time, so a scenario's waits scale with the run just as the driven
		// trajectory does.
		void Sleep(long a_ms);

		// Waits for the final pose, records the stats and stops the driver. Safe to call repeatedly.
		void Finish();

		// Null until a driver has run and finished.
		[[nodiscard]] const json& Stats() const { return m_stats; }

	private:
		const json&              m_steps;
		bool                     m_enabled;
		bool                     m_unavailable = false;
		std::unique_ptr<Session> m_session;
		std::optional<double>    m_deadlineGameMs;
		json                     m_stats;
	};
}
