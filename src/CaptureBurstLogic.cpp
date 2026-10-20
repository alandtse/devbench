#include "CaptureBurstLogic.h"

#include <algorithm>
#include <cmath>

namespace dvb::CaptureBurstLogic
{
	namespace
	{
		constexpr long             kBaseTimeoutMs = 5000;
		constexpr long             kPerFrameMs = 500;
		constexpr double           kUvSlack = 1.0e-6;
		constexpr std::string_view kProgressTopic = "capture.burst.progress";
		constexpr std::string_view kReadyTopic = "capture.burst.ready";

		bool IntField(const json& a_args, const char* a_name, int a_min, int a_max, int& a_out, std::string& a_error)
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
				return true;
			if (!it->is_number_integer() || it->get<std::int64_t>() < a_min || it->get<std::int64_t>() > a_max) {
				a_error = std::string("'") + a_name + "' must be an integer " + std::to_string(a_min) + ".." + std::to_string(a_max);
				return false;
			}
			a_out = it->get<int>();
			return true;
		}

		bool UnitField(const json& a_object, const char* a_name, double a_min, double a_max, double& a_out)
		{
			const auto it = a_object.find(a_name);
			if (it == a_object.end() || !it->is_number())
				return false;
			a_out = it->get<double>();
			return std::isfinite(a_out) && a_out >= a_min && a_out <= a_max;
		}

		int CountField(const json& a_payload, const char* a_name)
		{
			const auto it = a_payload.find(a_name);
			if (it == a_payload.end())
				return 0;
			if (it->is_array())
				return static_cast<int>(it->size());
			return it->is_number_integer() ? static_cast<int>(std::max<std::int64_t>(it->get<std::int64_t>(), 0)) : 0;
		}
	}

	std::optional<BurstRequest> ParseBurst(const json& a_args, std::string& a_error)
	{
		BurstRequest request;
		if (!a_args.contains("count")) {
			a_error = "burst requires 'count'";
			return std::nullopt;
		}
		if (!IntField(a_args, "count", 1, kMaxCount, request.count, a_error) || !IntField(a_args, "everyNFrames", 1, kMaxEveryNFrames, request.everyNFrames, a_error))
			return std::nullopt;

		if (const auto it = a_args.find("excludeUi"); it != a_args.end()) {
			if (!it->is_boolean()) {
				a_error = "'excludeUi' must be a boolean";
				return std::nullopt;
			}
			request.excludeUi = it->get<bool>();
		}

		if (const auto it = a_args.find("format"); it != a_args.end() && (!it->is_string() || it->get<std::string>() != "png")) {
			a_error = "'format' must be \"png\"";
			return std::nullopt;
		}

		if (a_args.contains("scale")) {
			a_error = "'scale' is not supported; crop with 'subrect'";
			return std::nullopt;
		}

		if (const auto it = a_args.find("subrect"); it != a_args.end()) {
			Subrect rect;
			if (!it->is_object() || !UnitField(*it, "x", 0.0, 1.0, rect.x) || !UnitField(*it, "y", 0.0, 1.0, rect.y) || !UnitField(*it, "w", kUvSlack, 1.0, rect.w) ||
				!UnitField(*it, "h", kUvSlack, 1.0, rect.h) || rect.x + rect.w > 1.0 + kUvSlack || rect.y + rect.h > 1.0 + kUvSlack) {
				a_error = "'subrect' must be {x,y,w,h} in 0..1 UV, w and h above 0, staying inside the frame";
				return std::nullopt;
			}
			request.subrect = rect;
		}

		request.timeoutMs = DefaultTimeoutMs(request.count, request.everyNFrames);
		if (const auto it = a_args.find("timeoutMs"); it != a_args.end()) {
			if (!it->is_number_integer() || it->get<std::int64_t>() < 1 || it->get<std::int64_t>() > kMaxTimeoutMs) {
				a_error = "'timeoutMs' must be an integer 1.." + std::to_string(kMaxTimeoutMs);
				return std::nullopt;
			}
			request.timeoutMs = it->get<long>();
		}
		return request;
	}

	long DefaultTimeoutMs(int a_count, int a_everyNFrames)
	{
		const auto frames = static_cast<long long>(std::max(a_count, 1)) * std::max(a_everyNFrames, 1);
		return static_cast<long>(std::min<long long>(kBaseTimeoutMs + frames * kPerFrameMs, kMaxTimeoutMs));
	}

	std::string_view StateName(State a_state)
	{
		switch (a_state) {
		case State::kRunning:
			return "running";
		case State::kDone:
			return "done";
		case State::kFailed:
			return "failed";
		case State::kTimeout:
			return "timeout";
		}
		return {};
	}

	bool ApplyEvent(RunState& a_run, std::string_view a_requestId, std::string_view a_topic, const json& a_payload)
	{
		if ((a_topic != kProgressTopic && a_topic != kReadyTopic) || !a_payload.is_object())
			return false;
		const auto id = a_payload.find("requestId");
		if (id == a_payload.end() || !id->is_string() || id->get<std::string>() != a_requestId)
			return false;
		if (a_run.state == State::kDone || a_run.state == State::kFailed)
			return true;

		a_run.captured = std::max(a_run.captured, CountField(a_payload, "captured"));
		a_run.written = std::max(a_run.written, CountField(a_payload, "written"));
		a_run.dropped = std::max(a_run.dropped, CountField(a_payload, "dropped"));
		if (a_topic == kProgressTopic) {
			a_run.state = State::kRunning;
			return true;
		}

		a_run.width = CountField(a_payload, "width");
		a_run.height = CountField(a_payload, "height");
		if (const auto it = a_payload.find("uiExcluded"); it != a_payload.end() && it->is_boolean())
			a_run.uiExcluded = it->get<bool>();
		if (const auto it = a_payload.find("manifestPath"); it != a_payload.end() && it->is_string())
			a_run.manifestPath = it->get<std::string>();
		const auto ok = a_payload.find("ok");
		if (ok != a_payload.end() && ok->is_boolean() && ok->get<bool>()) {
			a_run.state = State::kDone;
			a_run.error.clear();
			a_run.written = std::max(a_run.written, a_run.captured);
		} else {
			a_run.state = State::kFailed;
			const auto error = a_payload.find("error");
			a_run.error = error != a_payload.end() && error->is_string() ? error->get<std::string>() : "provider reported failure";
		}
		return true;
	}

	void ApplyTimeout(RunState& a_run, long a_elapsedMs, long a_timeoutMs)
	{
		if (a_run.state == State::kRunning && a_elapsedMs > a_timeoutMs) {
			a_run.state = State::kTimeout;
			a_run.error = "timed out waiting for the capture provider to finish the burst";
		}
	}

	bool IsBurstFrame(std::string_view a_name, std::string_view a_prefix, int a_count)
	{
		constexpr std::size_t      kIndexDigits = 4;
		constexpr std::string_view kExtension = ".png";
		if (a_name.size() != a_prefix.size() + 1 + kIndexDigits + kExtension.size() || !a_name.starts_with(a_prefix) ||
			a_name[a_prefix.size()] != '_' || !a_name.ends_with(kExtension))
			return false;
		int index = 0;
		for (const char c : a_name.substr(a_prefix.size() + 1, kIndexDigits)) {
			if (c < '0' || c > '9')
				return false;
			index = index * 10 + (c - '0');
		}
		return index < a_count;
	}

	json StatusJson(const RunState& a_run)
	{
		json out{
			{ "state", std::string(StateName(a_run.state)) },
			{ "captured", a_run.captured },
			{ "written", a_run.written },
			{ "dropped", a_run.dropped },
		};
		if (a_run.state == State::kDone || a_run.state == State::kFailed) {
			out["width"] = a_run.width;
			out["height"] = a_run.height;
			out["uiExcluded"] = a_run.uiExcluded;
		}
		if (!a_run.manifestPath.empty())
			out["manifestPath"] = a_run.manifestPath;
		if (!a_run.error.empty())
			out["error"] = a_run.error;
		return out;
	}
}
