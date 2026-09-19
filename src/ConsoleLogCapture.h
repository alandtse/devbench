#pragma once

#include <atomic>
#include <string>
#include <vector>

// Capture a single console command's output, by keeping devbench's OWN scrollback of the
// lines ConsoleLog prints and slicing it between two invalid "fence" marker commands the
// caller queues around the real command. RE::Console::ExecuteCommand is deferred (the GFx
// console drains queued commands on a later tick), so the begin marker, the command's
// output, and the end marker are printed in order; each marker echoes a
// "Script command \"<token>\" not found." line we slice between.
//
// ⛔ WHY WE KEEP OUR OWN SCROLLBACK, AND IT IS MEASURED, NOT ASSUMED (issue #83).
// This used to read `RE::ConsoleLog::GetSingleton()->buffer` and search it for the token, on
// the assumption - stated in this header, never tested - that `buffer` is "the accumulated
// scrollback". It is not. Instrumented and run on Skyrim SE 1.18.2 (vr:false, MO2), with a
// command that definitely prints:
//
//     bufferLen: 0   bufferEmpty: true   bufferHasBegin: false   consoleLogNull: false
//     lastMessage: Script command "DVBCAPENDx9F3" not found.
//
// `buffer` is EMPTY while the singleton is alive and `lastMessage` is correctly populated at
// the same instant, and the markers are plainly visible in the game's own console. The
// scrollback the player sees belongs to the console's Scaleform UI; `ConsoleLog` exposes only
// `char lastMessage[0x400]`, the single most recent line (CommonLibVR's own ConsoleLog.h:
// lastMessage at 0x001, BSString buffer at 0x408, no virtuals).
//
// ⚠ #83 IS FILED AS VR-ONLY AND THAT FRAMING IS WRONG. The reporter's symptom is
// `{count:0, lines:[], markersFound:false, sawBegin:false, sawEnd:false}` - byte for byte what
// SE returns. Nothing about VR is involved: the member being read is empty on both.
//
// NO DETOUR, DELIBERATELY: ConsoleLog::VPrint is an MSVC __try/SEH function
// (UNW_FLAG_UHANDLER), so a function-entry hook on it relocates the SEH prologue into a
// .pdata-less trampoline and CTDs when the console is opened. Sampling keeps devbench
// hook-free on this path and cannot crash the game.
//
// ⚠⚠ THE SAMPLER IS PACED BY THE CALLER, NOT BY RE-QUEUEING ITSELF, AND THAT COST A BUILD.
// The first version re-queued its own SKSE task each time. SKSE drains its task queue until
// it is empty within a frame, so a task that re-queues itself is run again immediately: a
// budget of 1800 "frames" was spent inside a fraction of one real frame, the console had not
// drained yet, and the capture came back `frames:1800, samples:0` while `lastMessage` held
// the end marker moments later. The loop lives on the listener thread now, one
// MainThread::RunAndWait per tick with a real sleep between, so a tick is a tick.
//
// ⚠ THE ONE LIMIT, AND IT IS NOT HIDDEN: the sampler observes `lastMessage` once per tick, so
// it records every line EXCEPT where two or more are printed between two ticks - then only
// the last of them is kept. Output that arrives a line at a time (`getav`, `getpos`, `getgs`,
// and every other printing command devbench is used for) is captured exactly. A command that
// dumps many lines at once (`help`) comes back short, and `sameFrameRisk` says so.
namespace dvb::ConsoleLogCapture
{
	// Invalid console commands used as fences - unique tokens unlikely to appear in normal
	// output; each echoes back inside a "Script command \"<token>\" not found." line.
	inline constexpr const char* kMarkerBegin = "DVBCAPBEGINx9F3";
	inline constexpr const char* kMarkerEnd = "DVBCAPENDx9F3";

	/// Lines kept. A fenced command's output is far smaller; the cap only bounds the ring if
	/// an end marker never arrives.
	inline constexpr std::size_t kRingMax = 512;

	struct Result
	{
		bool                     sawBegin = false;
		bool                     sawEnd = false;
		std::vector<std::string> lines;

		// --- diagnostics (issue #83), kept after the fix because they ARE the evidence for it:
		// on a healthy capture they still show bufferLen 0, which is the whole finding.
		bool        consoleLogNull = false;
		bool        bufferEmpty = false;
		std::size_t bufferLen = 0;
		bool        bufferHasBegin = false;
		std::string lastMessage;
		bool        lastMessageHasBegin = false;

		// --- sampler health ---
		std::size_t ringLines = 0;     ///< lines in devbench's scrollback
		std::size_t samples = 0;       ///< distinct lines recorded this capture
		std::size_t ticks = 0;         ///< times the sampler looked
		std::size_t engineFrames = 0;  ///< distinct ENGINE frames those ticks covered
		bool        sameFrameRisk = false;
		bool        timedOut = false;
	};

	/// Arm a capture: clear the scrollback and seed "already seen" with the line showing now.
	/// MUST run on the main thread, BEFORE the fence commands are queued.
	void BeginCapture();

	/// One look at ConsoleLog::lastMessage. MUST run on the main thread. Returns true once the
	/// end marker has been recorded, which is the caller's signal to stop.
	bool SampleOnce();

	/// Tell the capture it ran out of time rather than finishing. Main thread.
	void MarkTimedOut();

	/// Slice the most recent fenced window out of the scrollback. MUST run on the main thread.
	Result ReadFenced(size_t maxLines = 200);
}
