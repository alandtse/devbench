#pragma once

#include <array>
#include <optional>
#include <string>

#include "Json.h"

namespace dvb::MapLogic
{
	enum class Action
	{
		kGet,
		kLocate,
		kSetWaypoint,
		kClearWaypoint
	};

	struct Request
	{
		Action                              action = Action::kGet;
		std::string                         location;    ///< EditorID or FormID of a Location
		std::optional<std::array<float, 3>> position;    ///< x, y, z; z is 0 when omitted
		std::string                         worldspace;  ///< EditorID or FormID; only valid with a position
	};

	struct Parsed
	{
		std::optional<Request> request;
		std::string            error;  ///< set when request is empty
	};

	/// Validates the `map` tool arguments: a known action, and for locate/setWaypoint exactly one way to name the target.
	Parsed Parse(const json& a_args);
}
