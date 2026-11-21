#pragma once

#include <array>
#include <cstdint>

namespace dvb::VRControllerProps
{
	// The game's controller poll emits ButtonEvents only for bits in the supported-buttons property, so a
	// synthetic index must answer it or its state never becomes input events.
	inline constexpr std::uint32_t kSupportedButtonsProp = 3001;
	inline constexpr std::uint32_t kAxis0TypeProp = 3002;
	inline constexpr std::uint32_t kAxisCount = 5;

	inline constexpr std::int32_t kAxisNone = 0;
	inline constexpr std::int32_t kAxisJoystick = 2;
	inline constexpr std::int32_t kAxisTrigger = 3;

	inline constexpr std::uint64_t Bit(unsigned a_button) { return std::uint64_t{ 1 } << a_button; }

	// Oculus Touch layout.
	inline constexpr std::uint64_t kSyntheticSupportedButtons =
		Bit(1) | Bit(2) | Bit(7) | Bit(32) | Bit(33) | Bit(34) | Bit(35);

	inline constexpr std::int32_t SyntheticAxisType(std::uint32_t a_axis)
	{
		switch (a_axis) {
		case 0:
			return kAxisJoystick;
		case 1:
		case 2:
			return kAxisTrigger;
		default:
			return kAxisNone;
		}
	}

	/// Remembers the synthetic controller indices so a release edge still sees the supported bits for a short while
	/// after a sequence ends; the poll only reports an edge for a supported button.
	class RecentIndices
	{
	public:
		static constexpr std::int64_t kGraceMs = 250;

		void Note(std::uint32_t a_index, std::int64_t a_nowMs) noexcept
		{
			for (auto& slot : m_slots) {
				if (slot.valid && slot.index == a_index) {
					slot.seenMs = a_nowMs;
					return;
				}
			}
			auto& oldest = m_slots[0].seenMs <= m_slots[1].seenMs ? m_slots[0] : m_slots[1];
			oldest = { a_index, a_nowMs, true };
		}

		[[nodiscard]] bool Recent(std::uint32_t a_index, std::int64_t a_nowMs) const noexcept
		{
			for (const auto& slot : m_slots)
				if (slot.valid && slot.index == a_index)
					return a_nowMs - slot.seenMs <= kGraceMs;
			return false;
		}

	private:
		struct Slot
		{
			std::uint32_t index{ 0 };
			std::int64_t  seenMs{ 0 };
			bool          valid{ false };
		};
		std::array<Slot, 2> m_slots{};
	};
}
