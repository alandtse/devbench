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

	// Serializes a_value; text that is not valid UTF-8 (engine text that skipped FromGame) becomes
	// U+FFFD, because a throw here is an empty HTTP 500.
	inline std::string Dump(const json& a_value, int a_indent = -1)
	{
		return a_value.dump(a_indent, ' ', false, json::error_handler_t::replace);
	}

	/// a_text with any invalid UTF-8 replaced by U+FFFD, for strings the library will dump itself.
	inline std::string Utf8Scrub(const std::string& a_text)
	{
		return json::parse(Dump(json(a_text))).get<std::string>();
	}
}
