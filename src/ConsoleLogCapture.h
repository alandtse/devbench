#pragma once

#include "ConsoleCaptureLogic.h"

#include <string>
#include <vector>

// A fenced command's output is read from ConsoleLog::buffer while the game fills it, else from a
// sampler of ConsoleLog::lastMessage, which keeps only the last of several lines printed in one
// frame. The buffer stops being filled once the Console menu exists.
namespace dvb::ConsoleLogCapture
{
	struct Result
	{
		bool                     sawBegin = false;
		bool                     sawEnd = false;
		std::vector<std::string> lines;

		/// "buffer", "sampler", or "none" if no capture has seen its begin marker.
		std::string source = "none";
		/// Only the sampler can lose lines.
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

		std::size_t ringLines = 0;
		std::size_t samples = 0;
		std::size_t ticks = 0;
		std::size_t engineFrames = 0;
	};

	/// Runs `a_command` fenced and returns once its output has landed or the capture timed out.
	/// Listener thread only. Throws ToolError(409) if another capture is running.
	void RunFencedCapture(const std::string& a_command);

	/// Slices the last capture's output from the source it used. Main thread.
	Result ReadFenced(std::size_t a_maxLines = 200);
}
