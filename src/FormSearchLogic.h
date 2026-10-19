#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace dvb::FormSearchLogic
{
	inline constexpr std::size_t kDefaultLimit = 50;
	inline constexpr std::size_t kMaxLimit = 500;

	/// Case-insensitive substring test; an empty needle matches everything.
	bool ContainsCI(std::string_view a_haystack, std::string_view a_needle);

	/// Case-insensitive equality.
	bool EqualsCI(std::string_view a_left, std::string_view a_right);

	/// The head part type names, in engine order: Misc (0), Face, Eyes, Hair, FacialHair, Scar, Eyebrows.
	std::string_view HeadPartTypeName(int a_type);

	/// A head part type from its name (case-insensitive); nullopt for any other string.
	std::optional<int> ParseHeadPartType(std::string_view a_name);

	/// The armor slot numbers (30..61, the Creation Kit's numbering) set in a biped slot mask, ascending.
	std::vector<int> SlotsInMask(std::uint32_t a_mask);

	/// The bit for an armor slot number 30..61; nullopt outside it.
	std::optional<std::uint32_t> MaskForSlot(int a_slot);
}
