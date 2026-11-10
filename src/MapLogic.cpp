#include "MapLogic.h"

#include <cmath>
#include <format>
#include <initializer_list>
#include <utility>

namespace dvb::MapLogic
{
	namespace
	{
		Parsed Fail(std::string a_error)
		{
			return { std::nullopt, std::move(a_error) };
		}

		// nullopt when absent; NaN when present but not a finite float.
		std::optional<float> ReadCoordinate(const json& a_args, const char* a_key)
		{
			const auto it = a_args.find(a_key);
			if (it == a_args.end() || it->is_null())
				return std::nullopt;
			if (!it->is_number())
				return std::nanf("");
			const auto value = static_cast<float>(it->get<double>());
			return std::isfinite(value) ? value : std::nanf("");
		}

		std::optional<std::string> ReadText(const json& a_args, const char* a_key, bool& a_bad)
		{
			const auto it = a_args.find(a_key);
			if (it == a_args.end() || it->is_null())
				return std::nullopt;
			if (!it->is_string() || it->get_ref<const std::string&>().empty()) {
				a_bad = true;
				return std::nullopt;
			}
			return it->get<std::string>();
		}
	}

	Parsed Parse(const json& a_args)
	{
		const auto actionIt = a_args.find("action");
		if (actionIt == a_args.end() || !actionIt->is_string())
			return Fail("action is required: get, locate, setWaypoint or clearWaypoint");
		const auto& name = actionIt->get_ref<const std::string&>();

		Request request;
		if (name == "get")
			request.action = Action::kGet;
		else if (name == "locate")
			request.action = Action::kLocate;
		else if (name == "setWaypoint")
			request.action = Action::kSetWaypoint;
		else if (name == "clearWaypoint")
			request.action = Action::kClearWaypoint;
		else
			return Fail(std::format("unknown action '{}': use get, locate, setWaypoint or clearWaypoint", name));

		if (request.action == Action::kGet || request.action == Action::kClearWaypoint)
			return { request, {} };

		bool       bad = false;
		const auto location = ReadText(a_args, "location", bad);
		if (bad)
			return Fail("location must be a non-empty EditorID or FormID string");
		const auto worldspace = ReadText(a_args, "worldspace", bad);
		if (bad)
			return Fail("worldspace must be a non-empty EditorID or FormID string");

		const auto x = ReadCoordinate(a_args, "x");
		const auto y = ReadCoordinate(a_args, "y");
		const auto z = ReadCoordinate(a_args, "z");
		for (const auto& [key, value] : { std::pair{ "x", x }, std::pair{ "y", y }, std::pair{ "z", z } })
			if (value && std::isnan(*value))
				return Fail(std::format("{} must be a finite number", key));

		const bool hasPosition = x || y || z;
		if (location)
			request.location = *location;

		if (request.action == Action::kLocate) {
			if (!location)
				return Fail("locate needs a location (EditorID or FormID)");
			if (hasPosition || worldspace)
				return Fail("locate takes only a location");
			return { request, {} };
		}

		if (location && (hasPosition || worldspace))
			return Fail("give a location, or x and y with an optional z and worldspace, not both");
		if (!location) {
			if (!x || !y)
				return Fail("setWaypoint needs a location, or both x and y");
			request.position = std::array<float, 3>{ *x, *y, z.value_or(0.0f) };
			if (worldspace)
				request.worldspace = *worldspace;
		}
		return { request, {} };
	}
}
