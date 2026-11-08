#include "RunStatusLogic.h"

#include <algorithm>

namespace dvb::RunStatusLogic
{
	json Compact(const json& a_status, const json& a_args)
	{
		if (!a_status.is_object() || !a_status.contains("result") || !a_status["result"].is_object() ||
			!a_status["result"].contains("results") || !a_status["result"]["results"].is_array())
			return a_status;
		if (a_args.is_object() && a_args.value("verbose", false))
			return a_status;

		json       out = a_status;
		json&      result = out["result"];
		const json steps = std::move(result["results"]);
		result.erase("results");
		const std::size_t total = steps.size();
		result["resultsTotal"] = total;

		const bool paged = a_args.is_object() && (a_args.contains("from") || a_args.contains("limit"));
		if (paged) {
			const auto        from = std::min(total, static_cast<std::size_t>(std::max(0LL, a_args.value("from", 0LL))));
			const auto        limit = static_cast<std::size_t>(std::max(0LL, a_args.value("limit", static_cast<long long>(kDefaultPage))));
			const std::size_t end = std::min(total, from + limit);
			result["resultsFrom"] = from;
			result["results"] = json(steps.begin() + static_cast<std::ptrdiff_t>(from), steps.begin() + static_cast<std::ptrdiff_t>(end));
			return out;
		}

		json failures = json::array();
		for (const auto& step : steps) {
			if (failures.size() >= kMaxFailuresListed)
				break;
			if (step.is_object() && !step.value("ok", true))
				failures.push_back(step);
		}
		result["failures"] = std::move(failures);
		result["resultsHint"] = "steps omitted; pass verbose:true for all, or from/limit for a slice";
		return out;
	}
}
