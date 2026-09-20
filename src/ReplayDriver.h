#pragma once

#include "Json.h"
#include "ReplayTrajectory.h"

#include <chrono>
#include <memory>

namespace dvb::Recording::ReplayDriver
{
	// Drives the player along a trajectory from the game's main thread, once per engine frame,
	// with pose sampled from absolute elapsed time. Destroying the session stops it.
	class Session
	{
	public:
		virtual ~Session() = default;

		// Snapshot of playback quality: frames applied, frames skipped, worst gap between applies.
		[[nodiscard]] virtual json Stats() const = 0;

		// Blocks until the final pose has been applied. False if a_timeout passes first.
		virtual bool WaitFinished(std::chrono::milliseconds a_timeout) = 0;
	};

	// Begins driving immediately: elapsed time zero is the trajectory's first keyframe. Null when
	// the engine frame counter cannot be read, since the driver paces itself by it.
	std::unique_ptr<Session> Start(Trajectory a_trajectory);
}
