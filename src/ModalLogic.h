#pragma once

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// Which open menus stop an agent from acting, and what to tell it to do about each. Name-based on
// purpose: it runs on every tool reply and must not touch the main thread, which a pausing menu freezes.
namespace dvb::Modal
{
	struct Blocker
	{
		std::string name;
		std::string hint;
	};

	inline constexpr std::string_view kMessageBox = "MessageBoxMenu";

	/// Menus that are open during normal play and never block.
	inline bool IsBenign(std::string_view a_name)
	{
		static constexpr std::array<std::string_view, 7> kBenign = {
			"HUD Menu", "Cursor Menu", "Fader Menu", "Mist Menu", "LootMenu", "Console", "Overlay Interaction Menu"
		};
		return std::find(kBenign.begin(), kBenign.end(), a_name) != kBenign.end();
	}

	/// What the agent should do about a blocking menu.
	inline std::string HintFor(std::string_view a_name)
	{
		if (a_name == kMessageBox)
			return "menu action=describe to read it, then menu action=accept index=N (close does not dismiss a message box)";
		if (a_name == "Loading Menu")
			return "a load or door transition is in progress; poll menu action=list until it closes";
		if (a_name == "MainMenu" || a_name == "Main Menu")
			return "the game is at the main menu; load a save with game action=load or loadLast";
		return std::format("menu action=close name=\"{}\"", a_name);
	}

	/// The open menus that block, sorted by name, each with its hint.
	inline std::vector<Blocker> Blockers(const std::vector<std::string>& a_openMenus)
	{
		std::vector<Blocker> out;
		for (const auto& name : a_openMenus)
			if (!IsBenign(name))
				out.push_back({ name, HintFor(name) });
		std::sort(out.begin(), out.end(), [](const Blocker& a, const Blocker& b) { return a.name < b.name; });
		return out;
	}

	/// True if a_name may be auto-dismissed with a plain hide: a message box needs an answered button instead.
	inline bool CanAutoDismiss(std::string_view a_name)
	{
		return !a_name.empty() && a_name != kMessageBox && !IsBenign(a_name);
	}
}
