#include "BackgroundRun.h"

namespace dvb::BackgroundRun
{
	namespace
	{
		RE::Setting* Find()
		{
			return RE::GetINISetting("bAlwaysActive:General");
		}
	}

	std::optional<bool> AlwaysActive()
	{
		if (const auto* setting = Find())
			return setting->GetBool();
		return std::nullopt;
	}

	bool SetAlwaysActive(bool a_enabled)
	{
		auto* setting = Find();
		if (!setting)
			return false;
		setting->data.b = a_enabled;
		return true;
	}

	bool WindowFocused()
	{
		DWORD pid = 0;
		if (const HWND foreground = ::GetForegroundWindow())
			::GetWindowThreadProcessId(foreground, &pid);
		return pid == ::GetCurrentProcessId();
	}
}
