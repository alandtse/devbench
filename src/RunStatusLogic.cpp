#include "RunStatusLogic.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace dvb::RunStatusLogic
{
	namespace
	{
		// Values above size_t clamp instead of wrapping to a negative.
		std::size_t PageArg(const json& a_args, const char* a_name, std::size_t a_default)
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end() || !it->is_number_integer())
				return a_default;
			if (it->is_number_unsigned())
				return static_cast<std::size_t>(std::min<std::uint64_t>(it->get<std::uint64_t>(), std::numeric_limits<std::size_t>::max()));
			return static_cast<std::size_t>(std::max<std::int64_t>(it->get<std::int64_t>(), 0));
		}
	}

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
			const auto        from = std::min(total, PageArg(a_args, "from", 0));
			const auto        limit = PageArg(a_args, "limit", kDefaultPage);
			const std::size_t end = limit >= total - from ? total : from + limit;
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
