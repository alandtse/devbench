#include "ActorIdleLogic.h"

#include <algorithm>
#include <climits>
#include <format>

namespace dvb::ActorIdleLogic
{
	std::optional<std::int64_t> ParseRepeatMs(const json& a_value)
	{
		if (a_value.is_null())
			return kDefaultRepeatMs;
		if (!a_value.is_number_integer())
			return std::nullopt;
		const auto ms = a_value.get<std::int64_t>();
		if (ms < kRepeatFloorMs || ms > kRepeatCeilMs)
			return std::nullopt;
		return ms;
	}

	namespace
	{
		std::optional<float> ParseRange(const json& a_value, double a_default, double a_min, double a_max)
		{
			if (a_value.is_null())
				return static_cast<float>(a_default);
			if (!a_value.is_number())
				return std::nullopt;
			const auto v = a_value.get<double>();
			if (!(v >= a_min && v <= a_max))
				return std::nullopt;
			return static_cast<float>(v);
		}
	}

	std::optional<float> ParseLocomotionSpeed(const json& a_value, double a_default)
	{
		return ParseRange(a_value, a_default, 0.0, kMaxLocomotionSpeed);
	}

	namespace
	{
		constexpr double kSprintSpeed = 600.0;

		bool ValidName(const json& a_name)
		{
			return a_name.is_string() && !a_name.get_ref<const std::string&>().empty() &&
			       a_name.get_ref<const std::string&>().size() <= kMaxGaitNameLength;
		}

		void SetVariable(Gait& a_gait, std::string a_name, GraphValue a_value)
		{
			for (auto& existing : a_gait.variables) {
				if (existing.name == a_name) {
					existing.value = a_value;
					return;
				}
			}
			a_gait.variables.push_back({ std::move(a_name), a_value });
		}

		bool AddEvents(const json& a_args, const char* a_key, std::vector<std::string>& a_into, std::string& a_error)
		{
			if (!a_args.contains(a_key) || a_args[a_key].is_null())
				return true;
			const auto& list = a_args[a_key];
			if (!list.is_array() || list.size() > kMaxGaitEvents) {
				a_error = std::format("'{}' must be an array of at most {} graph event names", a_key, kMaxGaitEvents);
				return false;
			}
			for (const auto& name : list) {
				if (!ValidName(name)) {
					a_error = std::format("'{}' holds a name that is not a non-empty string of at most {} characters", a_key, kMaxGaitNameLength);
					return false;
				}
				if (std::ranges::find(a_into, name.get<std::string>()) == a_into.end())
					a_into.push_back(name.get<std::string>());
			}
			return true;
		}
	}

	std::optional<Gait> ParseGait(const json& a_args, std::string& a_error)
	{
		bool sprint = false;
		if (a_args.contains("sprint") && !a_args["sprint"].is_null()) {
			if (!a_args["sprint"].is_boolean()) {
				a_error = "'sprint' must be a boolean";
				return std::nullopt;
			}
			sprint = a_args["sprint"].get<bool>();
		}
		const auto speed = ParseLocomotionSpeed(a_args.contains("speed") ? a_args["speed"] : json(nullptr), sprint ? kSprintSpeed : kDefaultLocomotionSpeed);
		if (!speed) {
			a_error = "'speed' must be a number 0..1000";
			return std::nullopt;
		}

		Gait gait;
		gait.speed = *speed;
		if (sprint) {
			SetVariable(gait, "IsSprinting", true);
			SetVariable(gait, "iSyncSprintState", std::int32_t{ 1 });
			gait.startEvents.push_back("sprintStart");
			gait.stopEvents.push_back("sprintStop");
		}

		if (a_args.contains("variables") && !a_args["variables"].is_null()) {
			const auto& vars = a_args["variables"];
			if (!vars.is_object() || vars.size() > kMaxGaitVariables) {
				a_error = std::format("'variables' must be an object of at most {} graph variables", kMaxGaitVariables);
				return std::nullopt;
			}
			for (const auto& [name, value] : vars.items()) {
				if (name.empty() || name.size() > kMaxGaitNameLength) {
					a_error = std::format("a 'variables' name must be 1..{} characters", kMaxGaitNameLength);
					return std::nullopt;
				}
				if (value.is_boolean())
					SetVariable(gait, name, value.get<bool>());
				else if (value.is_number_integer() && value.get<std::int64_t>() >= INT32_MIN && value.get<std::int64_t>() <= INT32_MAX)
					SetVariable(gait, name, static_cast<std::int32_t>(value.get<std::int64_t>()));
				else if (value.is_number_float())
					SetVariable(gait, name, value.get<float>());
				else {
					a_error = std::format("variable '{}' must be a boolean, a 32-bit integer or a number", name);
					return std::nullopt;
				}
			}
			if (gait.variables.size() > kMaxGaitVariables) {
				a_error = std::format("at most {} graph variables are pinned", kMaxGaitVariables);
				return std::nullopt;
			}
		}
		if (!AddEvents(a_args, "events", gait.startEvents, a_error) || !AddEvents(a_args, "stopEvents", gait.stopEvents, a_error))
			return std::nullopt;
		return gait;
	}

	GraphValue NeutralValue(const GraphValue& a_value)
	{
		return std::visit(
			[](const auto& v) -> GraphValue {
				return std::remove_cvref_t<decltype(v)>{};
			},
			a_value);
	}

	bool ReplayDue(std::int64_t a_nowMs, std::int64_t a_lastPlayMs, std::int64_t a_repeatMs)
	{
		return a_nowMs - a_lastPlayMs >= a_repeatMs;
	}
}
