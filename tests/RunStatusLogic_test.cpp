#include "test_framework.h"

#include "RunStatusLogic.h"

using namespace dvb;
using namespace dvb::RunStatusLogic;

namespace
{
	json Status(int a_steps)
	{
		json results = json::array();
		for (int i = 0; i < a_steps; ++i)
			results.push_back(json{ { "index", i }, { "ok", i % 7 != 3 }, { "result", std::string(100, 'x') } });
		return json{ { "runId", 1 }, { "done", true }, { "ok", true }, { "result", json{ { "stepsRun", a_steps }, { "results", results } } } };
	}
}

TEST_CASE("a run status keeps its totals and failed steps, not the transcript")
{
	const json out = Compact(Status(30), json::object());
	CHECK(!out["result"].contains("results"));
	CHECK(out["result"]["resultsTotal"] == 30);
	CHECK(out["result"]["failures"].size() == 4);
	CHECK(out["result"]["stepsRun"] == 30);
}

TEST_CASE("verbose returns the whole transcript")
{
	const json in = Status(30);
	CHECK(Compact(in, json{ { "verbose", true } }) == in);
}

TEST_CASE("from and limit return that slice")
{
	const json out = Compact(Status(30), json{ { "from", 5 }, { "limit", 3 } });
	CHECK(out["result"]["results"].size() == 3);
	CHECK(out["result"]["results"][0]["index"] == 5);
	CHECK(out["result"]["resultsFrom"] == 5);
}

TEST_CASE("a slice past the end is empty, and a reply without a transcript is unchanged")
{
	CHECK(Compact(Status(3), json{ { "from", 99 } })["result"]["results"].empty());
	CHECK(Compact(Status(3), json{ { "from", 9223372036854775808ULL } })["result"]["results"].empty());
	CHECK(Compact(Status(3), json{ { "limit", 9223372036854775808ULL } })["result"]["results"].size() == 3);
	CHECK(Compact(Status(3), json{ { "from", 1 }, { "limit", 18446744073709551615ULL } })["result"]["results"].size() == 2);
	const json running{ { "runId", 2 }, { "done", false } };
	CHECK(Compact(running, json::object()) == running);
}
