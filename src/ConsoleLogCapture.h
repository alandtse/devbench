#pragma once

#include <string>
#include <vector>

// Capture a single console command's output by reading RE::ConsoleLog's accumulated text
// buffer and slicing the lines between two invalid "fence" marker commands the caller
// queues around the real command. RE::Console::ExecuteCommand is deferred (the GFx console
// drains queued commands on a later tick), so the begin marker, the command's output, and
// the end marker land in the buffer in order; each marker echoes a
// "Script command \"<token>\" not found." line we slice between.
//
// NO detour: ConsoleLog::VPrint is an MSVC __try/SEH function (UNW_FLAG_UHANDLER), so a
// function-entry hook on it relocates the SEH prologue into a .pdata-less trampoline and
// CTDs when the console is opened. Reading the game's own buffer avoids the hook entirely
// and keeps the console usable. Trade-off: on extreme ConsoleLog spam the begin marker can
// scroll out of the bounded buffer before `read`, in which case markersFound is false
// (graceful — no wrong data) rather than spam-immune as a hook-side fence would be.
namespace dvb::ConsoleLogCapture
{
	// Invalid console commands used as fences — unique tokens unlikely to appear in normal
	// output; each echoes back inside a "Script command \"<token>\" not found." line.
	inline constexpr const char* kMarkerBegin = "DVBCAPBEGINx9F3";
	inline constexpr const char* kMarkerEnd = "DVBCAPENDx9F3";

	struct Result
	{
		bool                     sawBegin = false;  ///< begin marker present in the buffer
		bool                     sawEnd = false;    ///< end marker present after it (window complete)
		std::vector<std::string> lines;             ///< lines between the markers, marker lines excluded
	};

	/// Read ConsoleLog's buffer and slice the most recent fenced window (between the last
	/// begin marker and the following end marker). MUST run on the main thread — the buffer
	/// is written by the game there. sawBegin && sawEnd → `lines` is exactly the command's
	/// output. Returns at most `maxLines` (most recent).
	Result ReadFenced(size_t maxLines = 200);
}
