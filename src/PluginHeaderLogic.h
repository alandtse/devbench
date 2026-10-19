#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dvb::PluginHeaderLogic
{
	struct PluginHeader
	{
		std::string              author;
		std::string              description;
		std::vector<std::string> masters;
		float                    version{ 0.0f };  // HEDR version: 0.94 / 1.7 for Skyrim
		std::uint32_t            recordCount{ 0 };
		bool                     master{ false };  // ESM flag
		bool                     light{ false };   // ESL flag
	};

	/// Reads the TES4 record at the start of a plugin file: author, description, masters and flags. Nullopt when the bytes do
	/// not begin with a TES4 record; it never reads outside a_bytes.
	std::optional<PluginHeader> Parse(std::span<const std::uint8_t> a_bytes);

	/// The bytes of the TES4 record a file needs to read in full: its 24-byte header plus its data. Nullopt when a_prefix is not
	/// the start of a TES4 record or the record is larger than a_limit.
	std::optional<std::size_t> RecordSize(std::span<const std::uint8_t> a_prefix, std::size_t a_limit);
}
