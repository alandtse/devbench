#include "HeldLightsLogic.h"

#include <unordered_map>

namespace dvb::HeldLights
{
	Summary Summarize(const std::vector<Search>& a_searches, const std::vector<Entry>& a_entries)
	{
		Summary out;
		out.complete = !a_searches.empty();
		for (const auto& s : a_searches)
			out.complete = out.complete && s.rootLoaded && s.nodeFound;

		std::unordered_map<std::uintptr_t, bool> unique;  // light -> in a scene list in any of its entries
		for (const auto& e : a_entries) {
			const bool inScene = e.membership != Membership::kNone;
			++out.entries;
			if (inScene)
				++out.entriesInScene;
			auto [it, added] = unique.try_emplace(e.light, inScene);
			if (!added)
				it->second = it->second || inScene;
		}
		out.uniqueLights = unique.size();
		for (const auto& [light, inScene] : unique)
			if (inScene)
				++out.uniqueLightsInScene;
		return out;
	}

	Membership MembershipOf(const char* a_list)
	{
		if (!a_list)
			return Membership::kNone;
		return std::string_view(a_list) == "shadow" ? Membership::kShadow : Membership::kActive;
	}

	json SummaryFields(const std::vector<Search>& a_searches, const Summary& a_summary, bool a_truncated)
	{
		json searched = json::array();
		for (const auto& s : a_searches)
			searched.push_back(json{ { "view", s.view }, { "node", s.node }, { "match", "firstByName" },
				{ "rootLoaded", s.rootLoaded }, { "nodeFound", s.nodeFound } });
		return json{
			{ "heldLightEntriesInScene", a_summary.entriesInScene },
			{ "heldLightsUnique", a_summary.uniqueLights },
			{ "heldLightsUniqueInScene", a_summary.uniqueLightsInScene },
			{ "heldLightSearch", std::move(searched) },
			{ "heldLightSearchComplete", a_summary.complete && !a_truncated },
		};
	}
}
