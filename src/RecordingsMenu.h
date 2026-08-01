#pragma once

// Optional in-game SKSE Menu Framework (SMF3) UI for browsing/managing the recording library.
// Inert unless SMF is installed. Lives in its own PCH-free TU (RecordingsMenu.cpp) because the
// SMF client header's cimgui typedefs cannot coexist with the real imgui.h the main PCH pulls in.
namespace dvb::UI
{
	// Register devbench's SMF pages. Call at kDataLoaded (SMF is up by then). No-op if SMF absent.
	void Register();

	// Register devbench's FUCK tool. Call at kDataLoaded. No-op if FUCK is not installed. Lives in
	// its own PCH-free TU (RecordingsMenuFuck.cpp) — FUCK_API.h pulls in the real imgui.h, which
	// cannot coexist with SMF's cimgui ImGuiMCP in RecordingsMenu.cpp.
	void RegisterFuck();
}
