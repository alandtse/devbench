#include "GfxLogic.h"

#include <algorithm>
#include <cctype>

namespace dvb::GfxLogic
{
	std::vector<std::string> SplitPath(const std::string& a_path)
	{
		std::vector<std::string> out;
		std::string              current;
		for (const char c : a_path) {
			if (c != '.') {
				current += c;
			} else if (!current.empty()) {
				out.push_back(std::move(current));
				current.clear();
			}
		}
		if (!current.empty())
			out.push_back(std::move(current));
		return out;
	}

	bool IsIndex(const std::string& a_segment)
	{
		return !a_segment.empty() && a_segment.size() < 10 &&
		       std::all_of(a_segment.begin(), a_segment.end(), [](unsigned char a_char) { return std::isdigit(a_char) != 0; });
	}
}
