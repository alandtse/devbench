#pragma once

#include <string>
#include <vector>

namespace dvb::GfxLogic
{
	/// Splits "_root.a.0.b" on dots. Empty segments are dropped, so a stray dot cannot address an empty member name.
	std::vector<std::string> SplitPath(const std::string& a_path);

	/// True for a segment that reads as an array index; short enough that std::stoul cannot overflow.
	bool IsIndex(const std::string& a_segment);
}
