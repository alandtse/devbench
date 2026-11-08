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
		// 0x80-0x9F; the five unassigned bytes keep their C1 code points, as Windows maps them.
		static constexpr char16_t k80[32] = { 0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
			0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D,
			0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178 };
		const auto                at = [&](std::size_t i) -> unsigned { return i < a_text.size() ? static_cast<unsigned char>(a_text[i]) : 0u; };
		const auto                in = [&](std::size_t i, unsigned lo, unsigned hi) { return at(i) >= lo && at(i) <= hi; };
		// Length of a valid UTF-8 sequence at i (RFC 3629, as dump() checks it), else 0: mixed text keeps its UTF-8 parts.
		const auto seq = [&](std::size_t i) -> std::size_t {
			const unsigned c = at(i);
			if (c >= 0xC2 && c <= 0xDF)
				return in(i + 1, 0x80, 0xBF) ? 2 : 0;
			if (c >= 0xE0 && c <= 0xEF) {
				const unsigned lo = c == 0xE0 ? 0xA0 : 0x80, hi = c == 0xED ? 0x9F : 0xBF;
				return in(i + 1, lo, hi) && in(i + 2, 0x80, 0xBF) ? 3 : 0;
			}
			if (c >= 0xF0 && c <= 0xF4) {
				const unsigned lo = c == 0xF0 ? 0x90 : 0x80, hi = c == 0xF4 ? 0x8F : 0xBF;
				return in(i + 1, lo, hi) && in(i + 2, 0x80, 0xBF) && in(i + 3, 0x80, 0xBF) ? 4 : 0;
			}
			return 0;
		};
		std::string out;
		out.reserve(a_text.size());
		for (std::size_t i = 0; i < a_text.size();) {
			const unsigned char c = static_cast<unsigned char>(a_text[i]);
			if (c < 0x80) {
				out += static_cast<char>(c);
				++i;
			} else if (const std::size_t n = seq(i)) {
				out.append(a_text, i, n);
				i += n;
			} else {
				const char16_t u = c < 0xA0 ? k80[c - 0x80] : c;
				if (u < 0x800) {
					out += static_cast<char>(0xC0 | (u >> 6));
				} else {
					out += static_cast<char>(0xE0 | (u >> 12));
					out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
				}
				out += static_cast<char>(0x80 | (u & 0x3F));
				++i;
			}
		}
		return out;
	}

	// The first a_maxBytes bytes of a_text, shortened so a multibyte character is never cut in half.
	inline std::string Utf8Prefix(const std::string& a_text, std::size_t a_maxBytes)
	{
		if (a_text.size() <= a_maxBytes)
			return a_text;
		std::size_t n = a_maxBytes;
		while (n > 0 && (static_cast<unsigned char>(a_text[n]) & 0xC0) == 0x80)
			--n;
		return a_text.substr(0, n);
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
