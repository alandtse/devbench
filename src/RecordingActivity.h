#pragma once

#include "Json.h"

#include <string>

namespace dvb::Recording
{
	// Host-independent contract and replay planner for the activity stream stored beside a
	// trajectory. The game-facing recorder creates the events; this module decides which of
	// them can be reproduced by the currently advertised input contract and interleaves those
	// transitions without changing the trajectory's recorded clock.
	json ActivityCaptureContract();
	json SummarizeActivity(const json& a_events);

	// Convert the synchronized OpenVR tracking stream plus legacy normalized controller events
	// into one coherent tracked-set sequence step. New recordings already carry exact controller
	// state in each tracking sample; older recording-3 captures are upgraded by inserting frames at
	// controller event timestamps and carrying the most recent pose forward.
	// Returns { step|null, report, inputOwner, durationMs }.
	json BuildVRTrackedSetReplay(const json& a_trackingSamples, const json& a_events,
		const std::string& a_inputOwner, bool a_replayInputs);

	// Returns { steps, report, inputOwner }. Keyboard button down/up transitions are interleaved
	// here. The synchronized VR tracked-set stream is assembled separately by
	// BuildVRTrackedSetReplay so both device families retain their own atomic timing contract.
	json InterleaveReplayableActivity(const json& a_steps, const json& a_events,
		const std::string& a_inputOwner, bool a_replayInputs);
}
