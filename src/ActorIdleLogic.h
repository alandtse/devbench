#pragma once

#include "Json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dvb::ActorIdleLogic
{
	inline constexpr std::int64_t kDefaultRepeatMs = 5000;
	inline constexpr std::int64_t kRepeatFloorMs = 250;
	inline constexpr std::int64_t kRepeatCeilMs = 60000;

	/// Milliseconds between replays of a looping idle: an integer within the floor..ceiling, absent -> the default; anything
	/// else is nullopt. The engine does not report when an idle ends, so a loop is timed: set this to the clip's length.
	std::optional<std::int64_t> ParseRepeatMs(const json& a_value);

	inline constexpr double      kDefaultLocomotionSpeed = 300.0;
	inline constexpr double      kMaxLocomotionSpeed = 1000.0;
	inline constexpr std::size_t kMaxGaitVariables = 16;
	inline constexpr std::size_t kMaxGaitEvents = 8;
	inline constexpr std::size_t kMaxGaitNameLength = 64;

	/// The movement speed for an in-place run: a number within 0..kMaxLocomotionSpeed, absent -> a_default; anything else is
	/// nullopt.
	std::optional<float> ParseLocomotionSpeed(const json& a_value, double a_default = kDefaultLocomotionSpeed);

	using GraphValue = std::variant<bool, std::int32_t, float>;

	struct GraphVariable
	{
		std::string name;
		GraphValue  value;
	};

	/// What an in-place run pins on the animation graph: the Speed channel, graph variables set again on every graph update,
	/// and one-shot graph events sent when the run starts and when it stops.
	struct Gait
	{
		float                      speed{ 0.0f };
		std::vector<GraphVariable> variables;
		std::vector<std::string>   startEvents;
		std::vector<std::string>   stopEvents;
	};

	/// Reads a gait from the tool arguments: 'speed', 'variables' (name -> bool, integer or number), 'events' and 'stopEvents'
	/// (graph event names), and the 'sprint' shorthand, which adds the sprint variables and events and a faster default speed.
	/// Explicit 'variables' override the shorthand's. On a malformed argument returns nullopt and sets a_error.
	std::optional<Gait> ParseGait(const json& a_args, std::string& a_error);

	/// The value a variable is returned to when its run ends: false, 0 or 0.0.
	GraphValue NeutralValue(const GraphValue& a_value);

	/// A looping idle is replayed once a_repeatMs has passed since it was last played.
	bool ReplayDue(std::int64_t a_nowMs, std::int64_t a_lastPlayMs, std::int64_t a_repeatMs);
}
