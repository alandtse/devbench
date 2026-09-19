#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Capture one console command's output by keeping devbench's own scrollback of the lines
// ConsoleLog prints, and slicing it between two invalid "fence" marker commands queued around
// the real command. ExecuteCommand is deferred, so the begin marker, the output and the end
// marker are printed in order; each marker echoes a
// "Script command \"<token>\" not found." line to slice between.
//
// ConsoleLog exposes only `char lastMessage[0x400]`, the most recent line. `buffer` is empty at
// runtime and is not the scrollback, so the lines here are sampled from `lastMessage`.
//
// INVARIANTS
//   * BeginCapture, SampleOnce and ReadFenced run on the MAIN THREAD. MarkTimedOut does not.
//   * A caller holds CaptureMutex() from BeginCapture until it stops sampling, so a second
//     request cannot clear the scrollback while the first is still filling it.
//   * Sampling looks at `lastMessage` once per tick, so two lines printed between ticks keep
//     only the last. One line per tick is exact, and nothing here can detect a line never seen.
namespace dvb::ConsoleLogCapture
{
	// Invalid console commands used as fences - unique tokens unlikely to appear in normal
	// output; each echoes back inside a "Script command \"<token>\" not found." line.
	inline constexpr const char* kMarkerBegin = "DVBCAPBEGINx9F3";
	inline constexpr const char* kMarkerEnd = "DVBCAPENDx9F3";

	/// Lines kept. A fenced command's output is far smaller; the cap only bounds the ring if an
	/// end marker never arrives.
	inline constexpr std::size_t kRingMax = 512;

	struct Result
	{
		bool                     sawBegin = false;
		bool                     sawEnd = false;
		std::vector<std::string> lines;

		// What each source held at read time.
		bool        consoleLogNull = false;
		bool        bufferEmpty = false;
		std::size_t bufferLen = 0;
		bool        bufferHasBegin = false;
		std::string lastMessage;
		bool        lastMessageHasBegin = false;

		// Raw sampler counts. They report what was observed and do not prove that no line was
		// missed; nothing should read them as a loss detector.
		std::size_t ringLines = 0;     ///< lines in devbench's scrollback
		std::size_t samples = 0;       ///< distinct lines recorded this capture
		std::size_t ticks = 0;         ///< times the sampler looked
		std::size_t engineFrames = 0;  ///< distinct engine frames those ticks covered

		/// The end marker was never sampled, so `lines` may be incomplete.
		bool timedOut = false;
	};

	/// Held by the caller for a whole capture, from BeginCapture until sampling stops.
	std::mutex& CaptureMutex();

	/// Arm a capture: clear the scrollback and seed "already seen" with the line showing now.
	/// Main thread, before the fence commands are queued.
	void BeginCapture();

	/// One look at ConsoleLog::lastMessage. Main thread. Returns true once the end marker has
	/// been recorded.
	bool SampleOnce();

	/// Record that a capture ended without its end marker. Any thread.
	void MarkTimedOut();

	/// Slice the most recent fenced window out of the scrollback. Main thread.
	Result ReadFenced(size_t maxLines = 200);
}
