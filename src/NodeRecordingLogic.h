#pragma once

#include "Json.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace dvb::NodeRecordingLogic
{
	inline constexpr std::size_t kMaxNodes = 16;
	inline constexpr std::size_t kMaxNameLength = 64;
	/// Frames kept in memory for one recording; sampling stops (and the recording reports it) at this count.
	inline constexpr std::size_t kMaxFrames = 36000;

	/// The node names to sample, from the `nodes` argument: a non-empty array of distinct, non-empty
	/// strings, at most kMaxNodes, each at most kMaxNameLength characters. On failure returns nullopt and
	/// sets a_error.
	std::optional<std::vector<std::string>> ParseNodeNames(const json& a_nodes, std::string& a_error);
}
