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
}
