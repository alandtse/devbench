#pragma once

#include <string>

#include "Json.h"

namespace dvb::ScenarioPolicy
{
	inline constexpr int kEmbeddedToolFallbackErrorCode = 422;

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
