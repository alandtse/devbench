#include "MenuExtensions.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>

namespace dvb::MenuExtensions
{
	namespace
	{
		std::string Lower(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}

		std::mutex                                   g_mutex;
		std::unordered_map<std::string, Entry>       g_byKey;        // lowercased name → entry
		std::unordered_map<std::string, std::string> g_displayName;  // lowercased name → original
	}

	bool Register(std::string a_menu, json a_descriptor, ToolHandler a_handler)
	{
		const std::string           key = Lower(a_menu);
		std::lock_guard<std::mutex> lock(g_mutex);
		const bool                  replaced = g_byKey.find(key) != g_byKey.end();
		g_byKey[key] = Entry{ std::move(a_descriptor), std::move(a_handler) };
		g_displayName[key] = std::move(a_menu);
		return !replaced;
	}

	std::optional<Entry> Find(std::string_view a_menu)
	{
		const std::string           key = Lower(std::string(a_menu));
		std::lock_guard<std::mutex> lock(g_mutex);
		const auto                  it = g_byKey.find(key);
		if (it == g_byKey.end())
			return std::nullopt;
		return it->second;
	}

	std::vector<std::string> Names()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		std::vector<std::string>    out;
		out.reserve(g_displayName.size());
		for (const auto& [key, name] : g_displayName)
			out.push_back(name);
		std::sort(out.begin(), out.end());
		return out;
	}
}
