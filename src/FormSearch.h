#pragma once

#include "Json.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace RE
{
	class Actor;
	class TESForm;
}

namespace dvb::FormSearch
{
	struct Query
	{
		std::string                  formType;  // Armor | ArmorAddon | HeadPart | Idle | NPC | Weapon | Misc
		std::string                  name;      // case-insensitive substring of the name or EditorID
		std::string                  model;     // case-insensitive substring of any model path
		std::string                  plugin;    // case-insensitive substring of the plugin the form comes from
		std::optional<std::uint32_t> slotMask;  // Armor / ArmorAddon: the biped slots that must all be set
		std::optional<int>           headPartType;
		std::size_t                  limit{ 50 };
		std::size_t                  offset{ 0 };
	};

	/// The form types a query can name, for error messages.
	const char* SupportedTypes();

	/// Main thread. Throws ToolError (400) for an unsupported formType or a filter the type does not have.
	json Find(const Query& a_query);

	/// Main thread. One form, described as Find describes it, with its plugin's header under pluginInfo (author, description,
	/// masters). An Idle also carries its animation (the clip its event plays, with
	/// its length) under animation; see AssetInfo::IdleAnimation.
	json DescribeForm(RE::TESForm* a_form);

	/// Main thread. The head parts on the actor's base record, each described as Find describes a HeadPart.
	json HeadPartsOf(RE::Actor* a_actor);
}
