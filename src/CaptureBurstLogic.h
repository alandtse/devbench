#pragma once

#include "Json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dvb::CaptureBurstLogic
{
	inline constexpr int  kMaxCount = 240;
	inline constexpr int  kMaxEveryNFrames = 600;
	inline constexpr long kMaxTimeoutMs = 600000;

	struct Subrect
	{
		double x{ 0.0 };
		double y{ 0.0 };
		double w{ 1.0 };
		double h{ 1.0 };
	};

	struct BurstRequest
	{
		int                    count{ 0 };
		int                    everyNFrames{ 1 };
		bool                   excludeUi{ true };
		std::optional<Subrect> subrect;
		long                   timeoutMs{ 0 };
	};

	/// Validates a `capture action='burst'` request. Nullopt with a_error set when a field is missing, not of its type, or
	/// outside its range.
	std::optional<BurstRequest> ParseBurst(const json& a_args, std::string& a_error);

	/// True when a_name is `<a_prefix>_NNNN.png` with a four-digit index below a_count: a frame this burst writes.
	bool IsBurstFrame(std::string_view a_name, std::string_view a_prefix, int a_count);

	/// How long a burst may run before it is reported as timed out: it presents count * everyNFrames frames, assumed to run at no
	/// less than two frames a second, plus a fixed allowance.
	long DefaultTimeoutMs(int a_count, int a_everyNFrames);

	enum class State
	{
		kRunning,
		kDone,
		kFailed,
		kTimeout,
	};

	std::string_view StateName(State a_state);

	/// What a provider has reported about one burst, folded from its events.
	struct RunState
	{
		State       state{ State::kRunning };
		int         captured{ 0 };
		int         written{ 0 };
		int         dropped{ 0 };
		int         width{ 0 };
		int         height{ 0 };
		bool        uiExcluded{ false };
		std::string error;
		std::string manifestPath;
	};

	/// Folds one provider event into a_run when it is a burst event for a_requestId; false when it is not. A finished burst
	/// stays finished: later events do not change it.
	bool ApplyEvent(RunState& a_run, std::string_view a_requestId, std::string_view a_topic, const json& a_payload);

	/// A burst still running at a_elapsedMs past its timeout becomes a timeout.
	void ApplyTimeout(RunState& a_run, long a_elapsedMs, long a_timeoutMs);

	json StatusJson(const RunState& a_run);
}
