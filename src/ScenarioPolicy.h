#pragma once

#include <string>

#include "Json.h"

namespace dvb::ScenarioPolicy
{
	/// Extension tools report domain failures in their JSON payload. A scenario
	/// must treat those as failed steps even when transport dispatch succeeded.
	[[nodiscard]] inline bool IsEmbeddedToolFailure(const json& a_value)
	{
		if (!a_value.is_object())
			return false;
		if (const auto error = a_value.find("error");
			error != a_value.end() && !error->is_null()) {
			return !error->is_string() || !error->get_ref<const std::string&>().empty();
		}
		if (const auto ok = a_value.find("ok");
			ok != a_value.end() && ok->is_boolean()) {
			return !ok->get<bool>();
		}
		return false;
	}

	[[nodiscard]] inline json EmbeddedToolErrorCode(const json& a_value)
	{
		if (a_value.is_object()) {
			if (const auto code = a_value.find("errorCode");
				code != a_value.end() && !code->is_null()) {
				return *code;
			}
		}
		return 422;
	}

	[[nodiscard]] inline std::string EmbeddedToolErrorMessage(const json& a_value)
	{
		if (a_value.is_object()) {
			if (const auto error = a_value.find("error"); error != a_value.end()) {
				return error->is_string() ? error->get<std::string>() : error->dump();
			}
		}
		return "tool returned ok:false";
	}
}
