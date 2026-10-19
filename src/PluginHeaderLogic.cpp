#include "PluginHeaderLogic.h"

#include <cstring>
#include <string_view>

namespace dvb::PluginHeaderLogic
{
	namespace
	{
		constexpr std::size_t   kRecordHeaderSize = 24;
		constexpr std::size_t   kSubrecordHeaderSize = 6;
		constexpr std::uint32_t kMasterFlag = 0x1;
		constexpr std::uint32_t kLightFlag = 0x200;

		bool IsTes4(std::span<const std::uint8_t> a_bytes)
		{
			return a_bytes.size() >= kRecordHeaderSize && std::memcmp(a_bytes.data(), "TES4", 4) == 0;
		}

		std::uint32_t U32(const std::uint8_t* a_at)
		{
			std::uint32_t value;
			std::memcpy(&value, a_at, sizeof(value));
			return value;
		}

		std::string Text(const std::uint8_t* a_at, std::size_t a_size)
		{
			std::string_view text{ reinterpret_cast<const char*>(a_at), a_size };
			if (const auto end = text.find('\0'); end != std::string_view::npos)
				text = text.substr(0, end);
			return std::string(text);
		}
	}

	std::optional<std::size_t> RecordSize(std::span<const std::uint8_t> a_prefix, std::size_t a_limit)
	{
		if (!IsTes4(a_prefix))
			return std::nullopt;
		const std::size_t size = kRecordHeaderSize + static_cast<std::size_t>(U32(a_prefix.data() + 4));
		return size <= a_limit ? std::optional<std::size_t>{ size } : std::nullopt;
	}

	std::optional<PluginHeader> Parse(std::span<const std::uint8_t> a_bytes)
	{
		if (!IsTes4(a_bytes))
			return std::nullopt;
		const auto dataSize = static_cast<std::size_t>(U32(a_bytes.data() + 4));
		const auto flags = U32(a_bytes.data() + 8);
		if (a_bytes.size() - kRecordHeaderSize < dataSize)
			return std::nullopt;

		PluginHeader header;
		header.master = (flags & kMasterFlag) != 0;
		header.light = (flags & kLightFlag) != 0;

		const auto* data = a_bytes.data() + kRecordHeaderSize;
		std::size_t at = 0;
		while (dataSize - at >= kSubrecordHeaderSize) {
			const auto*       sub = data + at;
			const std::size_t size = static_cast<std::size_t>(sub[4]) | (static_cast<std::size_t>(sub[5]) << 8);
			at += kSubrecordHeaderSize;
			if (dataSize - at < size)
				break;
			const std::string_view type{ reinterpret_cast<const char*>(sub), 4 };
			if (type == "HEDR" && size >= 12) {
				std::memcpy(&header.version, data + at, sizeof(float));
				header.recordCount = U32(data + at + 4);
			} else if (type == "CNAM") {
				header.author = Text(data + at, size);
			} else if (type == "SNAM") {
				header.description = Text(data + at, size);
			} else if (type == "MAST") {
				header.masters.push_back(Text(data + at, size));
			}
			at += size;
		}
		return header;
	}
}
