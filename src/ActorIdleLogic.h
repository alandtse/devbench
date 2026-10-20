#pragma once

#include "Json.h"

#include <cstdint>
#include <optional>

namespace dvb::ActorIdleLogic
{
	inline constexpr std::int64_t kDefaultRepeatMs = 5000;
	inline constexpr std::int64_t kRepeatFloorMs = 250;
	inline constexpr std::int64_t kRepeatCeilMs = 60000;

	/// Milliseconds between replays of a looping idle: an integer within the floor..ceiling, absent -> the default; anything
	/// else is nullopt. The engine does not report when an idle ends, so a loop is timed: set this to the clip's length.
	std::optional<std::int64_t> ParseRepeatMs(const json& a_value);

	/// A looping idle is replayed once a_repeatMs has passed since it was last played.
	bool ReplayDue(std::int64_t a_nowMs, std::int64_t a_lastPlayMs, std::int64_t a_repeatMs);
}
