#include "KeyboardInputState.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace dvb
{
	namespace
	{
		struct KeyAlias
		{
			std::uint16_t    scancode;
			std::string_view canonical;
			std::string_view aliases;
		};

		// DirectInput scan codes. `aliases` is pipe-delimited and normalized before matching;
		// separators/case therefore do not matter (page_up, Page Up, and page-up all work).
		constexpr std::array kAliases{
			KeyAlias{ 0x01, "escape", "esc" },
			KeyAlias{ 0x02, "1", "digit1" },
			KeyAlias{ 0x03, "2", "digit2" },
			KeyAlias{ 0x04, "3", "digit3" },
			KeyAlias{ 0x05, "4", "digit4" },
			KeyAlias{ 0x06, "5", "digit5" },
			KeyAlias{ 0x07, "6", "digit6" },
			KeyAlias{ 0x08, "7", "digit7" },
			KeyAlias{ 0x09, "8", "digit8" },
			KeyAlias{ 0x0A, "9", "digit9" },
			KeyAlias{ 0x0B, "0", "digit0" },
			KeyAlias{ 0x0C, "minus", "-" },
			KeyAlias{ 0x0D, "equals", "equal|=" },
			KeyAlias{ 0x0E, "backspace", "back" },
			KeyAlias{ 0x0F, "tab", "" },
			KeyAlias{ 0x10, "q", "" },
			KeyAlias{ 0x11, "w", "" },
			KeyAlias{ 0x12, "e", "" },
			KeyAlias{ 0x13, "r", "" },
			KeyAlias{ 0x14, "t", "" },
			KeyAlias{ 0x15, "y", "" },
			KeyAlias{ 0x16, "u", "" },
			KeyAlias{ 0x17, "i", "" },
			KeyAlias{ 0x18, "o", "" },
			KeyAlias{ 0x19, "p", "" },
			KeyAlias{ 0x1A, "leftBracket", "lbracket|[" },
			KeyAlias{ 0x1B, "rightBracket", "rbracket|]" },
			KeyAlias{ 0x1C, "enter", "return" },
			KeyAlias{ 0x1D, "leftControl", "lctrl|leftctrl|ctrl|control" },
			KeyAlias{ 0x1E, "a", "" },
			KeyAlias{ 0x1F, "s", "" },
			KeyAlias{ 0x20, "d", "" },
			KeyAlias{ 0x21, "f", "" },
			KeyAlias{ 0x22, "g", "" },
			KeyAlias{ 0x23, "h", "" },
			KeyAlias{ 0x24, "j", "" },
			KeyAlias{ 0x25, "k", "" },
			KeyAlias{ 0x26, "l", "" },
			KeyAlias{ 0x27, "semicolon", ";" },
			KeyAlias{ 0x28, "apostrophe", "quote|'" },
			KeyAlias{ 0x29, "grave", "backtick|tilde|`" },
			KeyAlias{ 0x2A, "leftShift", "lshift|shift" },
			KeyAlias{ 0x2B, "backslash", "\\" },
			KeyAlias{ 0x2C, "z", "" },
			KeyAlias{ 0x2D, "x", "" },
			KeyAlias{ 0x2E, "c", "" },
			KeyAlias{ 0x2F, "v", "" },
			KeyAlias{ 0x30, "b", "" },
			KeyAlias{ 0x31, "n", "" },
			KeyAlias{ 0x32, "m", "" },
			KeyAlias{ 0x33, "comma", "," },
			KeyAlias{ 0x34, "period", "dot|." },
			KeyAlias{ 0x35, "slash", "/" },
			KeyAlias{ 0x36, "rightShift", "rshift" },
			KeyAlias{ 0x37, "numpadMultiply", "multiply" },
			KeyAlias{ 0x38, "leftAlt", "lalt|alt" },
			KeyAlias{ 0x39, "space", "spacebar" },
			KeyAlias{ 0x3A, "capsLock", "caps" },
			KeyAlias{ 0x3B, "f1", "" },
			KeyAlias{ 0x3C, "f2", "" },
			KeyAlias{ 0x3D, "f3", "" },
			KeyAlias{ 0x3E, "f4", "" },
			KeyAlias{ 0x3F, "f5", "" },
			KeyAlias{ 0x40, "f6", "" },
			KeyAlias{ 0x41, "f7", "" },
			KeyAlias{ 0x42, "f8", "" },
			KeyAlias{ 0x43, "f9", "" },
			KeyAlias{ 0x44, "f10", "" },
			KeyAlias{ 0x45, "numLock", "" },
			KeyAlias{ 0x46, "scrollLock", "scroll" },
			KeyAlias{ 0x47, "numpad7", "" },
			KeyAlias{ 0x48, "numpad8", "" },
			KeyAlias{ 0x49, "numpad9", "" },
			KeyAlias{ 0x4A, "numpadSubtract", "subtract" },
			KeyAlias{ 0x4B, "numpad4", "" },
			KeyAlias{ 0x4C, "numpad5", "" },
			KeyAlias{ 0x4D, "numpad6", "" },
			KeyAlias{ 0x4E, "numpadAdd", "add" },
			KeyAlias{ 0x4F, "numpad1", "" },
			KeyAlias{ 0x50, "numpad2", "" },
			KeyAlias{ 0x51, "numpad3", "" },
			KeyAlias{ 0x52, "numpad0", "" },
			KeyAlias{ 0x53, "numpadDecimal", "decimal" },
			KeyAlias{ 0x57, "f11", "" },
			KeyAlias{ 0x58, "f12", "" },
			KeyAlias{ 0x9C, "numpadEnter", "" },
			KeyAlias{ 0x9D, "rightControl", "rctrl|rightctrl" },
			KeyAlias{ 0xB5, "numpadDivide", "divide" },
			KeyAlias{ 0xB8, "rightAlt", "ralt" },
			KeyAlias{ 0xC7, "home", "" },
			KeyAlias{ 0xC8, "up", "upArrow|arrowUp" },
			KeyAlias{ 0xC9, "pageUp", "pgup" },
			KeyAlias{ 0xCB, "left", "leftArrow|arrowLeft" },
			KeyAlias{ 0xCD, "right", "rightArrow|arrowRight" },
			KeyAlias{ 0xCF, "end", "" },
			KeyAlias{ 0xD0, "down", "downArrow|arrowDown" },
			KeyAlias{ 0xD1, "pageDown", "pgdn" },
			KeyAlias{ 0xD2, "insert", "ins" },
			KeyAlias{ 0xD3, "delete", "del" },
			KeyAlias{ 0xDB, "leftWindows", "lwin|leftWin" },
			KeyAlias{ 0xDC, "rightWindows", "rwin|rightWin" },
			KeyAlias{ 0xDD, "menu", "apps" },
		};

		std::string Normalize(std::string_view a_value)
		{
			std::string out;
			out.reserve(a_value.size());
			for (const unsigned char c : a_value) {
				if (c == ' ' || c == '-' || c == '_')
					continue;
				out.push_back(static_cast<char>(std::tolower(c)));
			}
			return out;
		}

		bool AliasMatches(std::string_view a_aliases, const std::string& a_wanted)
		{
			std::size_t start = 0;
			while (start <= a_aliases.size()) {
				const auto end = a_aliases.find('|', start);
				const auto part = a_aliases.substr(start,
					end == std::string_view::npos ? a_aliases.size() - start : end - start);
				if (!part.empty() && Normalize(part) == a_wanted)
					return true;
				if (end == std::string_view::npos)
					break;
				start = end + 1;
			}
			return false;
		}
	}

	std::optional<KeyboardKey> ResolveKeyboardKey(std::string_view a_name)
	{
		if (a_name == "-")
			return KeyboardKey{ 0x0C, "minus" };
		const std::string wanted = Normalize(a_name);
		if (wanted.empty())
			return std::nullopt;
		for (const auto& entry : kAliases) {
			if (Normalize(entry.canonical) == wanted || AliasMatches(entry.aliases, wanted))
				return KeyboardKey{ entry.scancode, std::string(entry.canonical) };
		}
		return std::nullopt;
	}

	std::optional<KeyboardKey> ResolveKeyboardKey(int a_scancode)
	{
		if (a_scancode <= 0 || a_scancode > 0xFF)
			return std::nullopt;
		for (const auto& entry : kAliases)
			if (entry.scancode == a_scancode)
				return KeyboardKey{ entry.scancode, std::string(entry.canonical) };
		return KeyboardKey{ static_cast<std::uint16_t>(a_scancode), "scancode-" + std::to_string(a_scancode) };
	}

	const std::vector<KeyboardKey>& KeyboardKeyCatalog()
	{
		static const std::vector<KeyboardKey> keys = [] {
			std::vector<KeyboardKey> out;
			out.reserve(kAliases.size());
			for (const auto& entry : kAliases)
				out.push_back({ entry.scancode, std::string(entry.canonical) });
			return out;
		}();
		return keys;
	}

	KeyboardAcquireResult KeyboardLeaseTable::Acquire(KeyboardKey a_key, std::string a_owner,
		std::int64_t a_nowMs, std::int64_t a_maxHoldMs)
	{
		if (auto current = Find(a_key.scancode)) {
			return { current->owner == a_owner ? KeyboardAcquireStatus::kAlreadyOwned : KeyboardAcquireStatus::kConflict,
				*current };
		}
		KeyboardLease lease{ std::move(a_key), std::move(a_owner), m_nextGeneration++,
			a_nowMs, a_nowMs + a_maxHoldMs };
		m_leases.push_back(lease);
		return { KeyboardAcquireStatus::kAcquired, std::move(lease) };
	}

	std::optional<KeyboardLease> KeyboardLeaseTable::Find(std::uint16_t a_scancode) const
	{
		const auto it = std::find_if(m_leases.begin(), m_leases.end(),
			[a_scancode](const KeyboardLease& a_lease) { return a_lease.key.scancode == a_scancode; });
		return it == m_leases.end() ? std::nullopt : std::optional<KeyboardLease>(*it);
	}

	std::optional<KeyboardLease> KeyboardLeaseTable::Remove(std::uint16_t a_scancode,
		std::string_view a_owner, bool a_force)
	{
		const auto it = std::find_if(m_leases.begin(), m_leases.end(),
			[a_scancode](const KeyboardLease& a_lease) { return a_lease.key.scancode == a_scancode; });
		if (it == m_leases.end() || (!a_force && it->owner != a_owner))
			return std::nullopt;
		KeyboardLease removed = *it;
		m_leases.erase(it);
		return removed;
	}

	std::optional<KeyboardLease> KeyboardLeaseTable::RemoveExact(std::uint16_t a_scancode,
		std::uint64_t                                                          a_generation)
	{
		const auto it = std::find_if(m_leases.begin(), m_leases.end(),
			[a_scancode, a_generation](const KeyboardLease& a_lease) {
				return a_lease.key.scancode == a_scancode && a_lease.generation == a_generation;
			});
		if (it == m_leases.end())
			return std::nullopt;
		KeyboardLease removed = *it;
		m_leases.erase(it);
		return removed;
	}

	std::vector<KeyboardLease> KeyboardLeaseTable::RemoveAll(std::optional<std::string_view> a_owner)
	{
		std::vector<KeyboardLease> removed;
		for (auto it = m_leases.begin(); it != m_leases.end();) {
			if (!a_owner || it->owner == *a_owner) {
				removed.push_back(*it);
				it = m_leases.erase(it);
			} else {
				++it;
			}
		}
		return removed;
	}

	std::vector<KeyboardLease> KeyboardLeaseTable::Snapshot() const
	{
		return m_leases;
	}
}
