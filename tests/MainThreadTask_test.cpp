#include "test_framework.h"

#include "MainThreadTask.h"

#include <latch>
#include <stdexcept>
#include <thread>

using dvb::MainThread::QueuedTask;
using namespace std::chrono_literals;

TEST_CASE("abandoned main-thread mutation never executes when the queue drains")
{
	int        mutations = 0;
	QueuedTask task([&]() { return ++mutations; }, QueuedTask::Clock::now() + 1min);
	auto       future = task.GetFuture();
	CHECK(task.Abandon());
	CHECK(task.Abandon());
	task.Run();
	CHECK(mutations == 0);
	CHECK(future.wait_for(0ms) == std::future_status::timeout);
}

TEST_CASE("main-thread queue enforces its deadline before the listener wakes")
{
	const auto deadline = QueuedTask::Clock::now();
	int        mutations = 0;
	QueuedTask task([&]() { return ++mutations; }, deadline);
	auto       future = task.GetFuture();
	task.Run(deadline);  // No listener-side Abandon call before queue dispatch.
	CHECK(mutations == 0);
	CHECK(task.Abandon());
	CHECK(future.wait_for(0ms) == std::future_status::timeout);
}

TEST_CASE("main-thread task preserves a completed result and executes once")
{
	int        mutations = 0;
	QueuedTask task([&]() { return ++mutations; }, QueuedTask::Clock::now() + 1min);
	auto       future = task.GetFuture();
	task.Run();
	CHECK(!task.Abandon());
	task.Run();
	CHECK(mutations == 1);
	CHECK(future.get() == 1);
}

TEST_CASE("main-thread task propagates the callback exception through its future")
{
	QueuedTask task([]() -> dvb::json { throw std::runtime_error("callback failure"); }, QueuedTask::Clock::now() + 1min);
	auto       future = task.GetFuture();
	CHECK_NOTHROW(task.Run());
	CHECK(!task.Abandon());
	bool expectedException = false;
	try {
		future.get();
	} catch (const std::runtime_error& error) {
		expectedException = std::string(error.what()) == "callback failure";
	}
	CHECK(expectedException);
}

TEST_CASE("abandoning an already running main-thread task does not interrupt it")
{
	std::latch   started(1);
	std::latch   finish(1);
	QueuedTask   task([&]() {
		started.count_down();
		finish.wait();
		return "completed";
	},
		QueuedTask::Clock::now() + 1min);
	auto         future = task.GetFuture();
	std::jthread gameThread([&]() { task.Run(); });
	started.wait();
	CHECK(!task.Abandon());
	CHECK(future.wait_for(0ms) == std::future_status::timeout);
	finish.count_down();
	gameThread.join();
	CHECK(future.get() == "completed");
}

TEST_CASE("concurrent abandonment and dispatch have exactly one winner")
{
	for (int attempt = 0; attempt < 100; ++attempt) {
		std::latch   ready(2);
		int          mutations = 0;
		QueuedTask   task([&]() { return ++mutations; }, QueuedTask::Clock::now() + 1min);
		auto         future = task.GetFuture();
		std::jthread gameThread([&]() {
			ready.arrive_and_wait();
			task.Run();
		});
		ready.arrive_and_wait();
		const bool abandoned = task.Abandon();
		gameThread.join();
		CHECK(mutations == (abandoned ? 0 : 1));
		if (!abandoned)
			CHECK(future.get() == 1);
	}
}
