#pragma once

#include <chrono>
#include <format>
#include <string>

namespace dvb::MainThread
{
	// Two frame observations describe only this request's wait interval. They
	// cannot distinguish cold startup/compilation, a pause, a long load or a hang,
	// and cannot establish that restarting the process is necessary.
	inline std::string DescribeTaskTimeout(std::chrono::milliseconds a_timeout, bool a_started,
		int a_frameAtStart = -1, int a_frameNow = -1)
	{
		if (a_started)
			return std::format("main-thread task did not finish within {}ms; it already started and may still complete; outcome uncertain, do not automatically replay", a_timeout.count());

		auto message = std::format("main-thread task did not start within {}ms; queued task abandoned before entry", a_timeout.count());
		if (a_frameAtStart < 0 || a_frameNow < 0)
			return message + "; frame progress unavailable; cause and recovery are unknown";
		if (a_frameAtStart == a_frameNow)
			return message + "; no frame advance observed during this wait; startup, loading/compilation, a full pause or a stall can delay task service; this observation does not establish a hang or a need to restart";
		// Do not subtract signed counters: a reset/wrap is not a positive elapsed
		// frame count or proof of a healthy, ready main-thread task queue.
		return message + std::format("; frame counter changed from {} to {} during this wait; task service was delayed, but cause and readiness are not established", a_frameAtStart, a_frameNow);
	}
}
