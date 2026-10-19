#pragma once

#include <string>

#include "Json.h"

namespace dvb::ScenarioPolicy
{
	inline constexpr int kEmbeddedToolFallbackErrorCode = 422;

	/// A replay holds the free camera so the recording's own camera transform drives the view. A replay that injects a
	/// VR tracked set must not: the injected HMD drives the real camera, and a held free camera would show the recorded
	/// game camera, which does not follow the head.
	[[nodiscard]] inline bool ShouldHoldReplayCamera(const json& a_vrTrackedSetReport)
	{
		if (!a_vrTrackedSetReport.is_object())
			return true;
		const auto frames = a_vrTrackedSetReport.find("emittedFrames");
		return frames == a_vrTrackedSetReport.end() || !frames->is_number_integer() || frames->get<std::int64_t>() <= 0;
	}

	/// Extension tools report domain failures in their JSON payload. A scenario
	/// must treat those as failed steps even when transport dispatch succeeded.
	[[nodiscard]] inline bool IsEmbeddedToolFailure(const json& a_value)
	{
		if (!a_value.is_object())
			return false;
		if (const auto error = a_value.find("error");
			error != a_value.end() && !error->is_null()) {
			if (!error->is_string() || !error->get_ref<const std::string&>().empty())
				return true;
		}
		if (const auto ok = a_value.find("ok");
			ok != a_value.end() && ok->is_boolean()) {
			return !ok->get<bool>();
		}
		return false;
	}

	/// Return the receipt's code or the scenario fallback failure code.
	[[nodiscard]] inline json EmbeddedToolErrorCode(const json& a_value)
	{
		if (a_value.is_object()) {
			if (const auto code = a_value.find("errorCode");
				code != a_value.end() && !code->is_null()) {
				return *code;
			}
		}
		return kEmbeddedToolFallbackErrorCode;
	}

	/// Return the receipt's message or a stable ok:false fallback.
	[[nodiscard]] inline std::string EmbeddedToolErrorMessage(const json& a_value)
	{
		if (a_value.is_object()) {
			if (const auto error = a_value.find("error");
				error != a_value.end() && !error->is_null()) {
				if (!error->is_string())
					return error->dump();
				if (!error->get_ref<const std::string&>().empty())
					return error->get<std::string>();
			}
		}
		return "tool returned ok:false";
	}
}
