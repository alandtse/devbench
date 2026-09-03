#include "RecordingActivity.h"

#include "VRInputState.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <stdexcept>
#include <vector>

namespace dvb::Recording
{
	namespace
	{
		constexpr std::int64_t kRecordedHoldCapMs = 60000;
		constexpr std::int64_t kRecordedHoldSafetyMarginMs = 2000;

		bool IsKeyboardTransition(const json& a_event)
		{
			if (a_event.value("kind", std::string{}) != "input" ||
				a_event.value("eventType", std::string{}) != "button" ||
				a_event.value("device", std::string{}) != "keyboard")
				return false;
			const std::string state = a_event.value("state", std::string{});
			const int         id = a_event.value("idCode", 0);
			return (state == "down" || state == "up") && id > 0 && id <= 255;
		}

		bool IsVRControllerEvent(const json& a_event)
		{
			if (a_event.value("kind", std::string{}) != "input")
				return false;
			const std::string device = a_event.value("device", std::string{});
			return device.starts_with("vive") || device.starts_with("oculus") ||
			       device.starts_with("wmr") || a_event.contains("wandIndex");
		}

		json EmptyControllerState()
		{
			return json{ { "packetNumber", 0 }, { "pressed", std::uint64_t{ 0 } },
				{ "touched", std::uint64_t{ 0 } },
				{ "axes", json::array({ json::array({ 0.0, 0.0 }), json::array({ 0.0, 0.0 }),
							  json::array({ 0.0, 0.0 }), json::array({ 0.0, 0.0 }),
							  json::array({ 0.0, 0.0 }) }) } };
		}

		std::string ControllerRole(const json& a_event, const json& a_frame, bool& a_fallback)
		{
			if (a_event.contains("wandIndex") && a_event["wandIndex"].is_number_integer()) {
				const int index = a_event["wandIndex"].get<int>();
				if (a_frame.value("left", json::object()).value("index", -1) == index)
					return "left";
				if (a_frame.value("right", json::object()).value("index", -1) == index)
					return "right";
			}
			const std::string device = a_event.value("device", std::string{});
			// Skyrim's Primary/Secondary labels do not preserve a physical role in old captures.
			// The default right-handed mapping is the only recoverable fallback; direct per-frame
			// controller state in new captures removes this ambiguity.
			if (device.ends_with("Primary")) {
				a_fallback = true;
				return "right";
			}
			if (device.ends_with("Secondary")) {
				a_fallback = true;
				return "left";
			}
			return {};
		}

		bool ApplyControllerEvent(json& a_state, const json& a_event, std::size_t& a_unsupported)
		{
			const std::string type = a_event.value("eventType", std::string{});
			if (type == "button") {
				const int id = a_event.value("idCode", -1);
				if (id < 0 || id >= 64) {
					++a_unsupported;
					return false;
				}
				const std::uint64_t bit = std::uint64_t{ 1 } << id;
				std::uint64_t       pressed = a_state.value("pressed", std::uint64_t{ 0 });
				std::uint64_t       touched = a_state.value("touched", std::uint64_t{ 0 });
				const std::string   state = a_event.value("state", std::string{});
				if (state == "down" || state == "held" ||
					(state == "changed" && a_event.value("value", 0.0) > 0.0)) {
					pressed |= bit;
					touched |= bit;  // pressed implies touched; old BSInput events did not retain touch separately
				} else if (state == "up" || a_event.value("value", 1.0) <= 0.0) {
					pressed &= ~bit;
					touched &= ~bit;
				} else {
					++a_unsupported;
					return false;
				}
				a_state["pressed"] = pressed;
				a_state["touched"] = touched;
				a_state["packetNumber"] = a_state.value("packetNumber", 0u) + 1;
				return true;
			}
			if (type == "thumbstick" && a_event.contains("x") && a_event.contains("y")) {
				if (!a_state.contains("axes") || !a_state["axes"].is_array() || a_state["axes"].size() != 5)
					a_state["axes"] = EmptyControllerState()["axes"];
				a_state["axes"][0] = json::array({ a_event.value("x", 0.0), a_event.value("y", 0.0) });
				a_state["packetNumber"] = a_state.value("packetNumber", 0u) + 1;
				return true;
			}
			++a_unsupported;
			return false;
		}

		json InputStep(const json& a_event, const std::string& a_owner)
		{
			json args{
				{ "action", a_event.value("state", std::string{}) },
				{ "device", "keyboard" },
				{ "key", a_event.value("idCode", 0) },
				{ "owner", a_owner },
			};
			if (args["action"] == "down")
				args["maxHoldMs"] = a_event.value("replayMaxHoldMs", kRecordedHoldCapMs);
			return json{ { "tool", "input" }, { "args", std::move(args) },
				{ "label", "recorded keyboard input" } };
		}

		void AppendWait(json& a_steps, std::int64_t a_ms)
		{
			if (a_ms <= 0)
				return;
			if (!a_steps.empty() && a_steps.back().is_object() && a_steps.back().size() == 1 &&
				a_steps.back().contains("wait")) {
				a_steps.back()["wait"] = a_steps.back()["wait"].get<std::int64_t>() + a_ms;
			} else {
				a_steps.push_back(json{ { "wait", a_ms } });
			}
		}
	}

	json ActivityCaptureContract()
	{
		return json{
			{ "name", "devbench.recording.activity" },
			{ "version", json{ { "major", 1 }, { "minor", 1 } } },
			{ "clock", "steadyMillisecondsFromRecordStart" },
			{ "ordering", "seq" },
			{ "inputLayer", "Skyrim.BSInputDeviceManager" },
			{ "normalized", true },
			{ "eventKinds", json::array({ "input", "menu", "lifecycle", "cell", "console" }) },
			{ "inputEventTypes", json::array({ "button", "mouseMove", "char", "thumbstick",
									 "deviceConnect", "kinect", "vrTouchpadPosition", "vrTouchpadSwipe", "unknown" }) },
			{ "replay", json{
							{ "keyboardButtonTransitions", true },
							{ "vrTrackedInputFrames", true },
							{ "controllerButtonTransitions", true },
							{ "analogControllerAxes", true },
							{ "pointerFromTrackedPoses", true },
							{ "legacyControllerUpgrade", "wand-index mapping; right-handed primary/secondary fallback" },
							{ "observationalEvents", false },
							{ "policy", "capture-all-replay-only-declared" },
						} },
		};
	}

	json SummarizeActivity(const json& a_events)
	{
		json counts{
			{ "total", 0 },
			{ "input", 0 },
			{ "menu", 0 },
			{ "lifecycle", 0 },
			{ "cell", 0 },
			{ "console", 0 },
			{ "keyboardTransitions", 0 },
			{ "vrControllerEvents", 0 },
			{ "unsupportedInput", 0 },
		};
		if (!a_events.is_array())
			return counts;
		counts["total"] = a_events.size();
		for (const auto& event : a_events) {
			const std::string kind = event.value("kind", std::string{});
			if (counts.contains(kind))
				counts[kind] = counts[kind].get<std::size_t>() + 1;
			if (kind == "input") {
				if (IsKeyboardTransition(event))
					counts["keyboardTransitions"] = counts["keyboardTransitions"].get<std::size_t>() + 1;
				else if (IsVRControllerEvent(event))
					counts["vrControllerEvents"] = counts["vrControllerEvents"].get<std::size_t>() + 1;
				else
					counts["unsupportedInput"] = counts["unsupportedInput"].get<std::size_t>() + 1;
			}
		}
		return counts;
	}

	json BuildVRTrackedSetReplay(const json& a_trackingSamples, const json& a_events,
		const std::string& a_inputOwner, bool a_replayInputs)
	{
		constexpr int kReplayTailMs = 50;
		json          report{
			{ "enabled", a_replayInputs },
			{ "sourceTrackingSamples", 0 },
			{ "emittedFrames", 0 },
			{ "convertedControllerEvents", 0 },
			{ "unsupportedControllerEvents", 0 },
			{ "roleFallbackEvents", 0 },
			{ "exactControllerStateSamples", 0 },
			{ "timestampAdjustedControllerEvents", 0 },
		};
		if (!a_replayInputs || !a_trackingSamples.is_array() || a_trackingSamples.empty())
			return json{ { "step", nullptr }, { "report", std::move(report) },
				{ "inputOwner", "" }, { "durationMs", 0 } };

		std::map<std::int64_t, json> timeline;
		for (std::size_t sampleIndex = 0; sampleIndex < a_trackingSamples.size(); ++sampleIndex) {
			const auto& sample = a_trackingSamples[sampleIndex];
			if (!sample.is_object() || !sample.contains("tMs") || !sample["tMs"].is_number_integer())
				throw std::invalid_argument(std::format("trackingSamples[{}].tMs must be an integer", sampleIndex));
			const auto tMs = ParseBoundedIntegerArgument(sample, "tMs", 0, 0,
				kMaximumVRTrackedDurationMs);
			if (timeline.contains(tMs))
				throw std::invalid_argument(std::format("trackingSamples[{}].tMs duplicates {}", sampleIndex, tMs));
			timeline.emplace(tMs, sample);
			report["sourceTrackingSamples"] = report["sourceTrackingSamples"].get<std::size_t>() + 1;
			if (sample.value("left", json::object()).contains("controller") &&
				sample.value("right", json::object()).contains("controller"))
				report["exactControllerStateSamples"] = report["exactControllerStateSamples"].get<std::size_t>() + 1;
		}
		if (timeline.empty())
			return json{ { "step", nullptr }, { "report", std::move(report) },
				{ "inputOwner", "" }, { "durationMs", 0 } };

		// The first complete set is defined at sequence time zero, even if the recorder's first
		// polling tick happened later. This prevents a null-HMD runtime leaking its placeholder pose.
		if (!timeline.contains(0)) {
			if (timeline.size() >= kMaximumVRTrackedFrames)
				throw std::invalid_argument("tracking samples leave no frame budget for the required time-zero pose");
			json initial = timeline.begin()->second;
			initial["tMs"] = 0;
			timeline.emplace(0, std::move(initial));
		}

		std::vector<json> controllerEvents;
		if (a_events.is_array())
			for (std::size_t eventIndex = 0; eventIndex < a_events.size(); ++eventIndex) {
				const auto& event = a_events[eventIndex];
				if (!IsVRControllerEvent(event))
					continue;
				if (!event.contains("tMs") || !event["tMs"].is_number_integer())
					throw std::invalid_argument(std::format("activityEvents[{}].tMs must be an integer", eventIndex));
				ParseBoundedIntegerArgument(event, "tMs", 0, 0,
					kMaximumVRTrackedDurationMs);
				controllerEvents.push_back(event);
			}
		std::stable_sort(controllerEvents.begin(), controllerEvents.end(), [](const json& a, const json& b) {
			const auto at = a.value("tMs", std::int64_t{ 0 });
			const auto bt = b.value("tMs", std::int64_t{ 0 });
			return at != bt ? at < bt : a.value("seq", 0ull) < b.value("seq", 0ull);
		});

		// Every ordered transition receives a distinct frame. Millisecond collisions are shifted
		// forward by the minimum amount required by the strictly increasing VR frame contract.
		std::int64_t lastAssignedEventMs = -1;
		std::size_t  adjustedEventTimes = 0;
		for (auto& event : controllerEvents) {
			if (timeline.size() >= kMaximumVRTrackedFrames)
				throw std::invalid_argument("tracking and controller events exceed the VR replay frame budget");
			const auto sourceMs = std::max<std::int64_t>(0, event.value("tMs", std::int64_t{ 0 }));
			auto       replayMs = std::max(sourceMs, lastAssignedEventMs + 1);
			while (timeline.contains(replayMs)) {
				if (replayMs >= kMaximumVRTrackedDurationMs)
					throw std::invalid_argument("controller transition timestamp adjustment exceeds the VR replay duration limit");
				++replayMs;
			}
			if (replayMs > kMaximumVRTrackedDurationMs)
				throw std::invalid_argument("controller transition timestamp adjustment exceeds the VR replay duration limit");
			if (replayMs != sourceMs)
				++adjustedEventTimes;
			event["replayTMs"] = replayMs;
			lastAssignedEventMs = replayMs;
			auto after = timeline.upper_bound(replayMs);
			if (after == timeline.begin())
				timeline.emplace(replayMs, after->second);
			else {
				--after;
				json held = after->second;
				held["tMs"] = replayMs;
				timeline.emplace(replayMs, std::move(held));
			}
		}
		report["timestampAdjustedControllerEvents"] = adjustedEventTimes;

		json          leftState = EmptyControllerState();
		json          rightState = EmptyControllerState();
		std::size_t   eventIndex = 0;
		std::size_t   unsupported = 0;
		std::size_t   converted = 0;
		std::size_t   fallback = 0;
		std::uint64_t frameSeq = 1;
		json          frames = json::array();
		for (auto& [tMs, frame] : timeline) {
			if (!frame.contains("hmd") || !frame.contains("left") || !frame.contains("right"))
				throw std::invalid_argument(std::format("tracking sample at {}ms must contain hmd, left, and right", tMs));
			if (frame["left"].contains("controller"))
				leftState = frame["left"]["controller"];
			if (frame["right"].contains("controller"))
				rightState = frame["right"]["controller"];
			while (eventIndex < controllerEvents.size() &&
				   controllerEvents[eventIndex].value("replayTMs", std::int64_t{ 0 }) <= tMs) {
				bool              roleFallback = false;
				const std::string role = ControllerRole(controllerEvents[eventIndex], frame, roleFallback);
				if (roleFallback)
					++fallback;
				if (role == "left")
					converted += ApplyControllerEvent(leftState, controllerEvents[eventIndex], unsupported) ? 1 : 0;
				else if (role == "right")
					converted += ApplyControllerEvent(rightState, controllerEvents[eventIndex], unsupported) ? 1 : 0;
				else
					++unsupported;
				++eventIndex;
			}
			frame["tMs"] = tMs;
			frame["seq"] = frameSeq++;
			frame["left"]["controller"] = leftState;
			frame["right"]["controller"] = rightState;
			frames.push_back(std::move(frame));
		}
		if (!frames.empty()) {
			const auto validated = ParseVRTrackedInputFrames(frames);
			frames = json::array();
			for (const auto& frame : validated)
				frames.push_back(VRTrackedInputFrameJson(frame));
		}
		const auto durationMs = frames.empty() ? 0 :
		                                         frames.back().value("tMs", std::int64_t{ 0 }) + kReplayTailMs;
		report["emittedFrames"] = frames.size();
		report["convertedControllerEvents"] = converted;
		report["unsupportedControllerEvents"] = unsupported;
		report["roleFallbackEvents"] = fallback;
		report["poseInterpolation"] = "previous-sample-hold-at-inserted-controller-event-frames";
		report["durationMs"] = durationMs;
		if (frames.empty())
			return json{ { "step", nullptr }, { "report", std::move(report) },
				{ "inputOwner", "" }, { "durationMs", 0 } };
		json step{ { "tool", "input" }, { "args", json{
													  { "action", "sequence" },
													  { "device", "vrTrackedSet" },
													  { "owner", a_inputOwner },
													  { "surviveLifecycle", true },
													  { "tailMs", kReplayTailMs },
													  { "frames", std::move(frames) },
												  } },
			{ "label", "recorded atomic HMD and controller input" } };
		return json{ { "step", std::move(step) }, { "report", std::move(report) },
			{ "inputOwner", a_inputOwner }, { "durationMs", durationMs } };
	}

	json InterleaveReplayableActivity(const json& a_steps, const json& a_events,
		const std::string& a_inputOwner, bool a_replayInputs)
	{
		json report = SummarizeActivity(a_events);
		report["enabled"] = a_replayInputs;
		report["replayedKeyboardTransitions"] = 0;
		report["skippedInput"] = report.value("input", 0);

		if (!a_replayInputs || !a_events.is_array())
			return json{ { "steps", a_steps }, { "report", std::move(report) }, { "inputOwner", "" } };

		std::vector<json> replayable;
		for (const auto& event : a_events)
			if (IsKeyboardTransition(event))
				replayable.push_back(event);
		std::stable_sort(replayable.begin(), replayable.end(), [](const json& a, const json& b) {
			const auto at = a.value("tMs", static_cast<std::int64_t>(0));
			const auto bt = b.value("tMs", static_cast<std::int64_t>(0));
			return at != bt ? at < bt : a.value("seq", 0ull) < b.value("seq", 0ull);
		});

		std::int64_t traceEndMs = 0;
		if (a_steps.is_array())
			for (const auto& step : a_steps) {
				traceEndMs = std::max(traceEndMs, step.value("atMs", traceEndMs));
				traceEndMs += std::max<std::int64_t>(0, step.value("wait", std::int64_t{ 0 }));
			}
		for (const auto& event : replayable)
			traceEndMs = std::max(traceEndMs, event.value("tMs", std::int64_t{ 0 }));

		// Derive each lease from its recorded up transition (or the trace end). Long
		// holds cannot be represented by the public 60-second safety lease, so reject
		// them explicitly instead of reporting a replay that released the key early.
		std::map<int, std::size_t> openKeys;
		for (std::size_t i = 0; i < replayable.size(); ++i) {
			const int         key = replayable[i].value("idCode", 0);
			const std::string state = replayable[i].value("state", std::string{});
			if (state == "down") {
				openKeys[key] = i;
			} else if (state == "up") {
				if (const auto it = openKeys.find(key); it != openKeys.end()) {
					const auto holdMs = std::max<std::int64_t>(0,
						replayable[i].value("tMs", std::int64_t{ 0 }) -
							replayable[it->second].value("tMs", std::int64_t{ 0 }));
					if (holdMs + kRecordedHoldSafetyMarginMs > kRecordedHoldCapMs)
						throw std::invalid_argument(std::format(
							"recorded keyboard hold for key {} is {}ms and exceeds the {}ms faithful replay limit",
							key, holdMs, kRecordedHoldCapMs - kRecordedHoldSafetyMarginMs));
					replayable[it->second]["replayMaxHoldMs"] =
						std::max<std::int64_t>(100, holdMs + kRecordedHoldSafetyMarginMs);
					openKeys.erase(it);
				}
			}
		}
		for (const auto& [key, index] : openKeys) {
			const auto holdMs = std::max<std::int64_t>(0, traceEndMs -
															  replayable[index].value("tMs", std::int64_t{ 0 }));
			if (holdMs + kRecordedHoldSafetyMarginMs > kRecordedHoldCapMs)
				throw std::invalid_argument(std::format(
					"unreleased recorded keyboard hold for key {} spans {}ms and exceeds the {}ms faithful replay limit",
					key, holdMs, kRecordedHoldCapMs - kRecordedHoldSafetyMarginMs));
			replayable[index]["replayMaxHoldMs"] =
				std::max<std::int64_t>(100, holdMs + kRecordedHoldSafetyMarginMs);
		}

		json         steps = json::array();
		std::size_t  eventIndex = 0;
		std::int64_t clockMs = 0;
		const auto   emitThrough = [&](std::int64_t a_endMs, json& a_out, std::int64_t& a_clock,
									   std::size_t& a_index) {
			while (a_index < replayable.size() &&
				   replayable[a_index].value("tMs", static_cast<std::int64_t>(0)) <= a_endMs) {
				const auto eventMs = std::max(a_clock,
					replayable[a_index].value("tMs", static_cast<std::int64_t>(0)));
				AppendWait(a_out, eventMs - a_clock);
				a_clock = eventMs;
				a_out.push_back(InputStep(replayable[a_index], a_inputOwner));
				++a_index;
			}
		};

		if (a_steps.is_array()) {
			for (const auto& original : a_steps) {
				const auto atMs = original.value("atMs", clockMs);
				if (atMs > clockMs) {
					emitThrough(atMs, steps, clockMs, eventIndex);
					AppendWait(steps, atMs - clockMs);
					clockMs = atMs;
				}
				emitThrough(clockMs, steps, clockMs, eventIndex);
				json       step = original;
				const auto waitMs = std::max<std::int64_t>(0, step.value("wait", static_cast<std::int64_t>(0)));
				step.erase("wait");
				// atMs is archival ground truth for later planners; the current scenario DSL uses
				// the established wait clock and must not receive a metadata-only step.
				step.erase("atMs");
				if (!step.empty())
					steps.push_back(std::move(step));
				const auto endMs = clockMs + waitMs;
				emitThrough(endMs, steps, clockMs, eventIndex);
				AppendWait(steps, endMs - clockMs);
				clockMs = endMs;
			}
		}
		while (eventIndex < replayable.size()) {
			const auto eventMs = std::max(clockMs,
				replayable[eventIndex].value("tMs", static_cast<std::int64_t>(0)));
			AppendWait(steps, eventMs - clockMs);
			clockMs = eventMs;
			steps.push_back(InputStep(replayable[eventIndex++], a_inputOwner));
		}

		report["replayedKeyboardTransitions"] = replayable.size();
		report["skippedInput"] = report.value("input", 0) - static_cast<int>(replayable.size());
		report["partial"] = report.value("skippedInput", 0) > 0;
		return json{ { "steps", std::move(steps) }, { "report", std::move(report) },
			{ "inputOwner", replayable.empty() ? "" : a_inputOwner } };
	}
}
