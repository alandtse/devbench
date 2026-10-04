#pragma once

#include "ConsoleCaptureLogic.h"

#include <string>
#include <vector>

// Output is read from a hook on ConsoleLog::VPrint that sees every line printed during the capture.
// Without the hook it falls back to ConsoleLog::buffer, which the game stops filling once the Console
// menu exists, else to a sampler of lastMessage that keeps only the last line printed per frame.
namespace dvb::ConsoleLogCapture
{
	struct Result
	{
		bool                     sawBegin = false;
		bool                     sawEnd = false;
		std::vector<std::string> lines;

		/// "print", "buffer", "sampler", or "none" if no capture has seen its begin marker.
		std::string source = "none";
		/// The sampler can lose lines; the print hook only past its line or byte cap, or for a print
		/// it could not format (all counted in printDropped).
		bool lossPossible = false;
		/// The end marker never arrived, so `lines` may be incomplete.
		bool timedOut = false;

		bool        consoleLogNull = false;
		bool        bufferEmpty = false;
		std::size_t bufferLen = 0;
		bool        bufferHasBegin = false;
		std::string lastMessage;
		bool        lastMessageHasBegin = false;
		bool        consoleMenuExists = false;
		bool        consoleMenuOpen = false;
		bool        consoleMode = false;

		bool        printHooked = false;
		std::size_t printLines = 0;
		std::size_t printBytes = 0;
		std::size_t printDropped = 0;

		std::size_t ringLines = 0;
		std::size_t samples = 0;
		std::size_t ticks = 0;
		std::size_t engineFrames = 0;
	};

	/// Hooks ConsoleLog::VPrint so captures see every printed line. Leaves the older sources in use
	/// when the function's first bytes are not a form it can safely relocate. Main thread, once.
	void InstallPrintHook();

	/// Runs `a_command` fenced; false if the end marker never arrived. Throws 409 if a capture is
	/// running and 504 if the begin marker never appeared (the command was not run). Listener thread only.
	bool RunFencedCapture(const std::string& a_command);

	/// Slices the last capture's output from the source it used. Main thread.
	Result ReadFenced(std::size_t a_maxLines = 200);
}
