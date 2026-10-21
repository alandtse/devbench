#pragma once

#include <optional>

namespace dvb::BackgroundRun
{
	/// The engine's [General] bAlwaysActive, or nullopt when the setting is not present.
	std::optional<bool> AlwaysActive();

	/// Sets bAlwaysActive in the running engine (not the INI file); false when the setting is absent.
	bool SetAlwaysActive(bool a_enabled);

	/// True when a window of this process is the foreground window.
	bool WindowFocused();
}
