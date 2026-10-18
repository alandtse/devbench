#include "DifficultyLogic.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace dvb::DifficultyLogic
{
	namespace
	{
		constexpr std::array<std::string_view, kLevelCount> kNames{ "Novice", "Apprentice", "Adept", "Expert", "Master", "Legendary" };

		bool EqualsIgnoreCase(std::string_view a_left, std::string_view a_right)
		{
			return std::ranges::equal(a_left, a_right, [](unsigned char l, unsigned char r) { return std::tolower(l) == std::tolower(r); });
		}
	}

	std::string_view LevelName(int a_level)
	{
		return a_level >= 0 && a_level < kLevelCount ? kNames[static_cast<std::size_t>(a_level)] : std::string_view{};
	}

	std::optional<int> ParseLevel(const json& a_value)
	{
		if (a_value.is_number_integer()) {
			const auto level = a_value.get<std::int64_t>();
			if (level >= 0 && level < kLevelCount)
				return static_cast<int>(level);
			return std::nullopt;
		}
		if (a_value.is_string()) {
			const auto name = a_value.get<std::string>();
			for (int i = 0; i < kLevelCount; ++i)
				if (EqualsIgnoreCase(name, kNames[static_cast<std::size_t>(i)]))
					return i;
		}
		return std::nullopt;
	}
}
