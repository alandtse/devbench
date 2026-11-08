#include "GfxLogic.h"

#include <algorithm>
#include <cctype>
#include <limits>

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

	std::optional<std::uint32_t> ReadLimit(const json& a_args, std::uint32_t a_default)
	{
		const auto it = a_args.find("limit");
		if (it == a_args.end())
			return a_default;
		if (!it->is_number_integer() || (!it->is_number_unsigned() && it->get<std::int64_t>() < 0))
			return std::nullopt;
		const auto value = it->get<std::uint64_t>();
		return value <= std::numeric_limits<std::uint32_t>::max() ? std::optional(static_cast<std::uint32_t>(value)) : std::nullopt;
	}

	namespace
	{
		std::size_t WellFormedLength(std::string_view a_text, std::size_t a_pos)
		{
			const auto lead = static_cast<unsigned char>(a_text[a_pos]);
			if (lead < 0x80)
				return 1;
			std::size_t   length = 0;
			std::uint32_t minimum = 0;
			std::uint32_t point = 0;
			if (lead >= 0xC2 && lead <= 0xDF) {
				length = 2;
				minimum = 0x80;
				point = lead & 0x1F;
			} else if (lead >= 0xE0 && lead <= 0xEF) {
				length = 3;
				minimum = 0x800;
				point = lead & 0x0F;
			} else if (lead >= 0xF0 && lead <= 0xF4) {
				length = 4;
				minimum = 0x10000;
				point = lead & 0x07;
			} else {
				return 0;
			}
			if (a_pos + length > a_text.size())
				return 0;
			for (std::size_t i = 1; i < length; ++i) {
				const auto next = static_cast<unsigned char>(a_text[a_pos + i]);
				if ((next & 0xC0) != 0x80)
					return 0;
				point = (point << 6) | (next & 0x3F);
			}
			const bool surrogate = point >= 0xD800 && point <= 0xDFFF;
			return point < minimum || surrogate || point > 0x10FFFF ? 0 : length;
		}

		bool IsValidUtf8(std::string_view a_text)
		{
			for (std::size_t pos = 0; pos < a_text.size();) {
				const auto length = WellFormedLength(a_text, pos);
				if (length == 0)
					return false;
				pos += length;
			}
			return true;
		}

		// Windows-1252 0x80-0x9F; the five undefined bytes map to their C1 control, as browsers do.
		constexpr std::uint16_t kCp1252High[32] = {
			0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
			0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
		};

		void AppendUtf8(std::string& a_out, std::uint32_t a_point)
		{
			if (a_point < 0x80) {
				a_out += static_cast<char>(a_point);
			} else if (a_point < 0x800) {
				a_out += static_cast<char>(0xC0 | (a_point >> 6));
				a_out += static_cast<char>(0x80 | (a_point & 0x3F));
			} else {
				a_out += static_cast<char>(0xE0 | (a_point >> 12));
				a_out += static_cast<char>(0x80 | ((a_point >> 6) & 0x3F));
				a_out += static_cast<char>(0x80 | (a_point & 0x3F));
			}
		}
	}

	std::string ToUtf8(std::string_view a_text)
	{
		if (IsValidUtf8(a_text))
			return std::string(a_text);
		std::string out;
		out.reserve(a_text.size() * 2);
		for (const char c : a_text) {
			const auto byte = static_cast<unsigned char>(c);
			AppendUtf8(out, byte >= 0x80 && byte <= 0x9F ? kCp1252High[byte - 0x80] : byte);
		}
		return out;
	}
}
