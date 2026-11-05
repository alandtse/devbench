#pragma once

// Single JSON type used across the registry, adapters, and handlers. We pull in
// nlohmann_json directly (the same library cpp-mcp uses); cpp-mcp's mcp_message.h
// is patched to include this same <nlohmann/json.hpp> so there is exactly one
// ABI-tagged namespace across our code and cpp-mcp's — no LNK2001 from a split.
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace dvb
{
	using json = nlohmann::json;

	// std::filesystem::path::string() converts wide->narrow using the Windows ANSI code
	// page, NOT UTF-8 — so a non-ASCII path (a CJK user profile, an accented install dir)
	// produces bytes nlohmann::json rejects on insertion, throwing type_error.316.
	// u8string() is UTF-8 by definition, so copy it into a std::string for json.
	inline std::string Utf8(const std::filesystem::path& a_path)
	{
		const std::u8string u8 = a_path.u8string();
		return { reinterpret_cast<const char*>(u8.data()), u8.size() };
	}

	inline std::string Utf8Text(const std::string& a_text)
	{
		try {
			(void)json(a_text).dump();
			return a_text;
		} catch (const json::type_error&) {}
		// 0x80-0x9F; the five unassigned bytes keep their C1 code points, as Windows maps them.
		static constexpr char16_t k80[32] = { 0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
			0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D,
			0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178 };
		std::string               out;
		for (const unsigned char c : a_text) {
			const char16_t u = c >= 0x80 && c < 0xA0 ? k80[c - 0x80] : c;
			if (u < 0x80) {
				out += static_cast<char>(u);
			} else if (u < 0x800) {
				out += static_cast<char>(0xC0 | (u >> 6));
				out += static_cast<char>(0x80 | (u & 0x3F));
			} else {
				out += static_cast<char>(0xE0 | (u >> 12));
				out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (u & 0x3F));
			}
		}
		return out;
	}

	inline std::string Dump(const json& a_value, int a_indent = -1)
	{
		try {
			return a_value.dump(a_indent);
		} catch (const json::type_error& e) {
			if (e.id != 316)
				throw;
		}
		json       fixed = a_value;
		const auto recode = [](const auto& a_self, json& a_j) -> void {
			if (a_j.is_string())
				a_j = Utf8Text(a_j.get_ref<const std::string&>());
			else if (a_j.is_structured())
				for (auto& v : a_j)
					a_self(a_self, v);
		};
		recode(recode, fixed);
		return fixed.dump(a_indent, ' ', false, json::error_handler_t::replace);  // keys
	}
}
