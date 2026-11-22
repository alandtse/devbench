#include "ActorIdleLogic.h"

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

	std::optional<float> ParseLocomotionSpeed(const json& a_value, bool a_sprint)
	{
		return ParseRange(a_value, a_sprint ? kDefaultSprintSpeed : kDefaultLocomotionSpeed, 0.0, kMaxLocomotionSpeed);
	}

	bool ReplayDue(std::int64_t a_nowMs, std::int64_t a_lastPlayMs, std::int64_t a_repeatMs)
	{
		return a_nowMs - a_lastPlayMs >= a_repeatMs;
	}
}
