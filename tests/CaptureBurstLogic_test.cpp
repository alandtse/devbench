#include "test_framework.h"

#include "CaptureBurstLogic.h"

using namespace dvb;
using namespace dvb::CaptureBurstLogic;

namespace
{
	std::optional<BurstRequest> Parse(const json& a_args, std::string& a_error)
	{
		return ParseBurst(a_args, a_error);
	}
}

TEST_CASE("a burst needs a count and takes the defaults for the rest")
{
	std::string error;
	const auto  request = Parse(json{ { "count", 60 } }, error);
	CHECK(request.has_value());
	if (!request)
		return;
	CHECK(request->count == 60);
	CHECK(request->everyNFrames == 1);
	CHECK(request->excludeUi);
	CHECK(!request->subrect.has_value());
	CHECK(request->timeoutMs == DefaultTimeoutMs(60, 1));

	CHECK(!Parse(json::object(), error).has_value());
	CHECK(error == "burst requires 'count'");
}

TEST_CASE("count and the frame step stay within their limits")
{
	std::string error;
	CHECK(Parse(json{ { "count", kMaxCount } }, error).has_value());
	CHECK(!Parse(json{ { "count", 0 } }, error).has_value());
	CHECK(!Parse(json{ { "count", kMaxCount + 1 } }, error).has_value());
	CHECK(!Parse(json{ { "count", 1.5 } }, error).has_value());
	CHECK(!Parse(json{ { "count", "60" } }, error).has_value());
	CHECK(Parse(json{ { "count", 1 }, { "everyNFrames", kMaxEveryNFrames } }, error).has_value());
	CHECK(!Parse(json{ { "count", 1 }, { "everyNFrames", 0 } }, error).has_value());
	CHECK(!Parse(json{ { "count", 1 }, { "everyNFrames", kMaxEveryNFrames + 1 } }, error).has_value());
}

TEST_CASE("format, excludeUi and timeout are validated and scale is refused")
{
	std::string error;
	CHECK(!Parse(json{ { "count", 1 }, { "scale", 0.5 } }, error).has_value());
	CHECK(error == "'scale' is not supported; crop with 'subrect'");
	CHECK(Parse(json{ { "count", 1 }, { "format", "png" } }, error).has_value());
	CHECK(!Parse(json{ { "count", 1 }, { "format", "gif" } }, error).has_value());
	CHECK(!Parse(json{ { "count", 1 }, { "excludeUi", 1 } }, error).has_value());
	CHECK(Parse(json{ { "count", 1 }, { "excludeUi", false } }, error)->excludeUi == false);
	CHECK(Parse(json{ { "count", 1 }, { "timeoutMs", 1000 } }, error)->timeoutMs == 1000);
	CHECK(!Parse(json{ { "count", 1 }, { "timeoutMs", 0 } }, error).has_value());
	CHECK(!Parse(json{ { "count", 1 }, { "timeoutMs", kMaxTimeoutMs + 1 } }, error).has_value());
}

TEST_CASE("a subrect is a rectangle inside the unit square")
{
	std::string error;
	const auto  ok = Parse(json{ { "count", 1 }, { "subrect", json{ { "x", 0.25 }, { "y", 0.1 }, { "w", 0.5 }, { "h", 0.9 } } } }, error);
	CHECK(ok.has_value());
	if (ok) {
		CHECK(ok->subrect->x == 0.25);
		CHECK(ok->subrect->h == 0.9);
	}
	CHECK(Parse(json{ { "count", 1 }, { "subrect", json{ { "x", 0 }, { "y", 0 }, { "w", 1 }, { "h", 1 } } } }, error).has_value());
	CHECK(!Parse(json{ { "count", 1 }, { "subrect", json{ { "x", 0.6 }, { "y", 0 }, { "w", 0.5 }, { "h", 1 } } } }, error).has_value());
	CHECK(!Parse(json{ { "count", 1 }, { "subrect", json{ { "x", 0 }, { "y", 0 }, { "w", 0 }, { "h", 1 } } } }, error).has_value());
	CHECK(!Parse(json{ { "count", 1 }, { "subrect", json{ { "x", -0.1 }, { "y", 0 }, { "w", 1 }, { "h", 1 } } } }, error).has_value());
	CHECK(!Parse(json{ { "count", 1 }, { "subrect", json{ { "x", 0 }, { "y", 0 }, { "w", 1 } } } }, error).has_value());
	CHECK(!Parse(json{ { "count", 1 }, { "subrect", "all" } }, error).has_value());
}

TEST_CASE("the default timeout grows with the frames a burst presents and is capped")
{
	CHECK(DefaultTimeoutMs(1, 1) == 5500);
	CHECK(DefaultTimeoutMs(60, 1) == 35000);
	CHECK(DefaultTimeoutMs(60, 4) == 125000);
	CHECK(DefaultTimeoutMs(kMaxCount, kMaxEveryNFrames) == kMaxTimeoutMs);
}

TEST_CASE("progress and ready events for the burst's request fold into its state")
{
	RunState run;
	CHECK(ApplyEvent(run, "r1", "capture.burst.progress", json{ { "requestId", "r1" }, { "captured", 10 }, { "written", 8 }, { "dropped", 1 } }));
	CHECK(run.state == State::kRunning);
	CHECK(run.captured == 10);
	CHECK(run.written == 8);
	CHECK(run.dropped == 1);

	CHECK(ApplyEvent(run, "r1", "capture.burst.ready",
		json{ { "requestId", "r1" }, { "ok", true }, { "captured", 60 }, { "written", 59 }, { "dropped", 1 }, { "width", 1280 }, { "height", 720 }, { "uiExcluded", true }, { "manifestPath", "C:/m.json" } }));
	CHECK(run.state == State::kDone);
	CHECK(run.captured == 60);
	CHECK(run.written == 60);
	CHECK(run.width == 1280);
	CHECK(run.uiExcluded);
	CHECK(run.manifestPath == "C:/m.json");
}

TEST_CASE("events for another request, other topics and malformed payloads are ignored")
{
	RunState run;
	CHECK(!ApplyEvent(run, "r1", "capture.burst.ready", json{ { "requestId", "r2" }, { "ok", true } }));
	CHECK(!ApplyEvent(run, "r1", "capture.ready", json{ { "requestId", "r1" }, { "ok", true } }));
	CHECK(!ApplyEvent(run, "r1", "capture.burst.ready", json{ { "ok", true } }));
	CHECK(!ApplyEvent(run, "r1", "capture.burst.ready", json("done")));
	CHECK(run.state == State::kRunning);
}

TEST_CASE("a dropped list in a ready event counts its entries")
{
	RunState run;
	ApplyEvent(run, "r1", "capture.burst.ready", json{ { "requestId", "r1" }, { "ok", true }, { "captured", 3 }, { "dropped", json::array({ 4, 9 }) } });
	CHECK(run.dropped == 2);
}

TEST_CASE("a failed ready event carries its error; a ready without ok is a failure")
{
	RunState run;
	CHECK(ApplyEvent(run, "r1", "capture.burst.ready", json{ { "requestId", "r1" }, { "ok", false }, { "error", "device lost" } }));
	CHECK(run.state == State::kFailed);
	CHECK(run.error == "device lost");

	RunState bare;
	CHECK(ApplyEvent(bare, "r1", "capture.burst.ready", json{ { "requestId", "r1" } }));
	CHECK(bare.state == State::kFailed);
	CHECK(bare.error == "provider reported failure");
}

TEST_CASE("a finished burst is not changed by later events, and only a running one times out")
{
	RunState run;
	ApplyEvent(run, "r1", "capture.burst.ready", json{ { "requestId", "r1" }, { "ok", true }, { "captured", 5 } });
	CHECK(ApplyEvent(run, "r1", "capture.burst.progress", json{ { "requestId", "r1" }, { "captured", 99 } }));
	CHECK(run.state == State::kDone);
	CHECK(run.captured == 5);
	ApplyTimeout(run, 999999, 10);
	CHECK(run.state == State::kDone);

	RunState slow;
	ApplyTimeout(slow, 100, 1000);
	CHECK(slow.state == State::kRunning);
	ApplyTimeout(slow, 1001, 1000);
	CHECK(slow.state == State::kTimeout);
	CHECK(!slow.error.empty());

	ApplyEvent(slow, "r1", "capture.burst.ready", json{ { "requestId", "r1" }, { "ok", true } });
	CHECK(slow.state == State::kDone);
	CHECK(slow.error.empty());
}

TEST_CASE("only this burst's exact frame names are counted")
{
	CHECK(IsBurstFrame("shot_0000.png", "shot", 3));
	CHECK(IsBurstFrame("shot_0002.png", "shot", 3));
	CHECK(!IsBurstFrame("shot_0003.png", "shot", 3));
	CHECK(!IsBurstFrame("shot_000.png", "shot", 3));
	CHECK(!IsBurstFrame("shot_00001.png", "shot", 3));
	CHECK(!IsBurstFrame("shot_00a1.png", "shot", 3));
	CHECK(!IsBurstFrame("shot_0001.jpg", "shot", 3));
	CHECK(!IsBurstFrame("shot2_0001.png", "shot", 3));
	CHECK(!IsBurstFrame("shot_manifest.json", "shot", 3));
}

TEST_CASE("the status json reports the state, counts and, once finished, the frame size")
{
	RunState running;
	running.captured = 3;
	const json live = StatusJson(running);
	CHECK(live["state"] == "running");
	CHECK(live["captured"] == 3);
	CHECK(!live.contains("width"));
	CHECK(!live.contains("error"));

	RunState done;
	ApplyEvent(done, "r1", "capture.burst.ready", json{ { "requestId", "r1" }, { "ok", true }, { "width", 640 }, { "height", 360 }, { "manifestPath", "m.json" } });
	const json finished = StatusJson(done);
	CHECK(finished["state"] == "done");
	CHECK(finished["width"] == 640);
	CHECK(finished["manifestPath"] == "m.json");
}
