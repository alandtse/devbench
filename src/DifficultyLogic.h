#pragma once

#include "Json.h"

#include <optional>
#include <string_view>

namespace dvb::DifficultyLogic
{
	inline constexpr int kLevelCount = 6;

	/// The difficulty names, in engine order: Novice (0) up to Legendary (5).
	std::string_view LevelName(int a_level);

	/// An integer 0..5 or a level name (case-insensitive); anything else is nullopt.
	std::optional<int> ParseLevel(const json& a_value);
}
