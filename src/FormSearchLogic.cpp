#include "FormSearchLogic.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace dvb::FormSearchLogic
{
	namespace
	{
		constexpr std::array<std::string_view, 7> kHeadPartTypes{ "Misc", "Face", "Eyes", "Hair", "FacialHair", "Scar", "Eyebrows" };
		constexpr int                             kFirstSlot = 30;
		constexpr int                             kSlotCount = 32;

	}

	bool EqualsCI(std::string_view a_left, std::string_view a_right)
	{
		return std::ranges::equal(a_left, a_right, [](unsigned char l, unsigned char r) { return std::tolower(l) == std::tolower(r); });
	}

	bool ContainsCI(std::string_view a_haystack, std::string_view a_needle)
	{
		if (a_needle.empty())
			return true;
		const auto it = std::ranges::search(a_haystack, a_needle, [](unsigned char l, unsigned char r) { return std::tolower(l) == std::tolower(r); });
		return !it.empty();
	}

	std::string_view HeadPartTypeName(int a_type)
	{
		return a_type >= 0 && static_cast<std::size_t>(a_type) < kHeadPartTypes.size() ? kHeadPartTypes[static_cast<std::size_t>(a_type)] : std::string_view{};
	}

	std::optional<int> ParseHeadPartType(std::string_view a_name)
	{
		for (std::size_t i = 0; i < kHeadPartTypes.size(); ++i)
			if (EqualsCI(a_name, kHeadPartTypes[i]))
				return static_cast<int>(i);
		return std::nullopt;
	}

	std::vector<int> SlotsInMask(std::uint32_t a_mask)
	{
		std::vector<int> slots;
		for (int bit = 0; bit < kSlotCount; ++bit)
			if (a_mask & (std::uint32_t{ 1 } << bit))
				slots.push_back(kFirstSlot + bit);
		return slots;
	}

	std::optional<std::uint32_t> MaskForSlot(int a_slot)
	{
		if (a_slot < kFirstSlot || a_slot >= kFirstSlot + kSlotCount)
			return std::nullopt;
		return std::uint32_t{ 1 } << (a_slot - kFirstSlot);
	}
}
