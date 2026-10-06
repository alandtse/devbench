#pragma once

#include "Json.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dvb::HeldLights
{
	/// Which of the world ShadowSceneNode's lists a light was found in. Membership is not proof that the light is
	/// visible or lights anything.
	enum class Membership
	{
		kNone,
		kActive,
		kShadow,
	};

	/// One view's search for the hand's node: the first node with the conventional name ("WEAPON" / "SHIELD")
	/// under that view's 3D root.
	struct Search
	{
		std::string view;  // "thirdPerson" / "firstPerson"
		std::string node;
		bool        rootLoaded = false;
		bool        nodeFound = false;
	};

	/// One light found under a searched node. The same light can appear once per view, or more than once when
	/// the graph shares it.
	struct Entry
	{
		std::uintptr_t light = 0;  // identity of the NiLight
		Membership     membership = Membership::kNone;
	};

	struct Summary
	{
		std::size_t entries = 0;
		std::size_t entriesInScene = 0;
		std::size_t uniqueLights = 0;
		std::size_t uniqueLightsInScene = 0;
		/// Every searched view's root was loaded and its node found, so an empty result means none were found there.
		/// A view the actor has no separate 3D for is not searched (Summarize never sees it).
		bool complete = false;
	};

	Summary Summarize(const std::vector<Search>& a_searches, const std::vector<Entry>& a_entries);

	/// a_list is the scene list name the active-light index holds for a light ("active" / "shadow"), or null when
	/// the light is in neither.
	Membership MembershipOf(const char* a_list);

	/// The hand's held-light fields that sit beside its `heldLights` array.
	json SummaryFields(const std::vector<Search>& a_searches, const Summary& a_summary, bool a_truncated);
}
