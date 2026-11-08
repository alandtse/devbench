#pragma once

#include "Json.h"

namespace dvb::RunStatusLogic
{
	inline constexpr std::size_t kMaxFailuresListed = 10;
	inline constexpr std::size_t kDefaultPage = 20;

	/// Shrinks a run-status reply ({done, ok?, result?:{results:[…]}}). A transcript can run to hundreds of steps, so
	/// by default `results` becomes `resultsTotal` plus the failed steps; `verbose:true` returns it whole, and
	/// `from`/`limit` return that slice of it. A reply without a transcript is returned unchanged.
	json Compact(const json& a_status, const json& a_args);
}
