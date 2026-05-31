#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Captures the output of a single console command by detouring RE::ConsoleLog::VPrint
// — the sink every command's output funnels through (verified across SE/AE/VR; its
// caller Print has 600+ command-handler callers). RE::Console::ExecuteCommand is void
// and runs DEFERRED (the GFx console drains queued commands on a later tick), so output
// cannot be bracketed synchronously.
//
// HOOK-SIDE FENCE: the caller enqueues the real command between two invalid marker
// commands (kMarkerBegin/kMarkerEnd); each echoes its token back in a "not found" line.
// While a capture window is open, the detour records ONLY the lines between those two
// markers and DROPS everything else. This is essential on heavy modlists where some
// mod spams ConsoleLog every frame (millions of lines): a capture-all-then-slice ring
// is flooded and the markers + output are evicted before they can be read. By fencing
// inside the hook, the spam outside the markers is never buffered, so it cannot evict
// the command's output. When no window is open the detour is a near-no-op (one atomic).
namespace dvb::ConsoleLogCapture
{
	// Invalid console commands used as fences — unique tokens unlikely to appear in
	// normal output; each echoes back inside a "Script command \"<token>\" not found"
	// line when executed through the (deferred) console.
	inline constexpr const char* kMarkerBegin = "DVBCAPBEGINx9F3";
	inline constexpr const char* kMarkerEnd = "DVBCAPENDx9F3";

	struct Line
	{
		uint64_t seq = 0;    ///< monotonic, assigned in capture order
		uint32_t frame = 0;  ///< render frame at capture time (0 if unavailable)
		std::string text;    ///< one console line, trailing newline stripped
	};

	/// Install the VPrint detour. Idempotent; call once from the hook-install path
	/// (the SKSE trampoline must have space).
	void Install();

	/// Clear the buffer and arm capture. The detour begins recording when it next sees
	/// the begin marker and stops at the end marker. Call before enqueuing the fenced
	/// command(s); read closes the window.
	void BeginCapture();

	/// Disarm capture; the detour returns to a near-no-op.
	void EndCapture();

	bool IsCapturing();

	/// Whether the current/last window observed each marker — both true means a
	/// complete fence was captured (the buffer holds exactly the command's output).
	bool SawBegin();
	bool SawEnd();

	/// Sequence number the next captured (between-marker) line will receive.
	uint64_t HeadSeq();

	/// Up to maxLines most-recent captured lines, oldest-first.
	std::vector<Line> Snapshot(size_t maxLines);
}
