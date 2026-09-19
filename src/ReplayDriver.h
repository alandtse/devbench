#pragma once

#include "Json.h"
#include "ReplayTrajectory.h"

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
	};

	// Begins driving immediately: elapsed time zero is the trajectory's first keyframe.
	std::unique_ptr<Session> Start(Trajectory a_trajectory);
}
