#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Json.h"

namespace dvb::GfxLogic
{
	/// Splits "_root.a.0.b" on dots. Empty segments are dropped, so a stray dot cannot address an empty member name.
	std::vector<std::string> SplitPath(const std::string& a_path);

	/// True for a segment that reads as an array index; short enough that std::stoul cannot overflow.
	bool IsIndex(const std::string& a_segment);

	/// Counts the values a read has emitted against a cap.
	struct Budget
	{
		int  nodes = 0;
		bool truncated = false;

		/// True, and marks the read truncated, once a_max nodes are spent.
		bool Exhausted(int a_max)
		{
			if (nodes < a_max)
				return false;
			truncated = true;
			return true;
		}
	};

	/// The optional `limit` argument: a_default when absent, nullopt unless it is an integer in 0..UINT32_MAX.
	std::optional<std::uint32_t> ReadLimit(const json& a_args, std::uint32_t a_default);

	/// a_text as UTF-8: unchanged when already valid, otherwise read as Windows-1252, which Scaleform menu text can carry.
	std::string ToUtf8(std::string_view a_text);
}
