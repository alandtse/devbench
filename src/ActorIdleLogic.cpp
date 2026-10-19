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

	bool ReplayDue(std::int64_t a_nowMs, std::int64_t a_lastPlayMs, std::int64_t a_repeatMs)
	{
		return a_nowMs - a_lastPlayMs >= a_repeatMs;
	}
}
