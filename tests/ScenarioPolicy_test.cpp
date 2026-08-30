#include "test_framework.h"

#include "ScenarioPolicy.h"

using dvb::json;

TEST_CASE("scenario rejects embedded extension errors")
{
	const json result{
		{ "error", "qualification_wait requires object parameter 'target'" },
		{ "errorCode", "invalid_target" },
	};

	CHECK(dvb::ScenarioPolicy::IsEmbeddedToolFailure(result));
	CHECK(dvb::ScenarioPolicy::EmbeddedToolErrorCode(result) == "invalid_target");
	CHECK(dvb::ScenarioPolicy::EmbeddedToolErrorMessage(result) ==
		  "qualification_wait requires object parameter 'target'");
}

TEST_CASE("scenario rejects explicit ok false receipts")
{
	const json result{ { "ok", false }, { "errors", json::array({ "failed" }) } };

	CHECK(dvb::ScenarioPolicy::IsEmbeddedToolFailure(result));
	CHECK(dvb::ScenarioPolicy::EmbeddedToolErrorCode(result) == 422);
}

TEST_CASE("scenario evaluates ok false after an empty error")
{
	const json result{ { "error", "" }, { "ok", false } };

	CHECK(dvb::ScenarioPolicy::IsEmbeddedToolFailure(result));
	CHECK(dvb::ScenarioPolicy::EmbeddedToolErrorMessage(result) == "tool returned ok:false");
}

TEST_CASE("scenario uses the fallback message for a null error")
{
	const json result{ { "error", nullptr }, { "ok", false } };

	CHECK(dvb::ScenarioPolicy::IsEmbeddedToolFailure(result));
	CHECK(dvb::ScenarioPolicy::EmbeddedToolErrorMessage(result) == "tool returned ok:false");
}

TEST_CASE("semantic waiter outcomes remain successful tool steps")
{
	const json result{
		{ "satisfied", false },
		{ "outcome", "timeout" },
		{ "failureReasons", json::array({ "profile_pending" }) },
	};

	CHECK(!dvb::ScenarioPolicy::IsEmbeddedToolFailure(result));
}
