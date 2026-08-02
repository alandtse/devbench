#include "MainThread.h"

#include "GameState.h"     // dvb::game::CurrentFrame
#include "ToolRegistry.h"  // dvb::ToolError

#include <algorithm>
#include <atomic>
#include <future>
#include <memory>

namespace
{
	// Liveness markers read off the listener thread (via MainThread::LastCompletedFrame /
	// PendingTasks) so /api/health can tell "task queue draining" from "starved" without a
	// RunAndWait round-trip: g_lastTaskFrame is the engine frame when the most recent queued
	// task finished ON the main thread; g_pendingTasks is queued-but-not-yet-finished.
	std::atomic<int> g_lastTaskFrame{ -1 };
	std::atomic<int> g_pendingTasks{ 0 };
}

namespace dvb::MainThread
{
	json RunAndWait(std::function<json()> a_fn, std::chrono::milliseconds a_timeout, const std::atomic<bool>* a_keepWaiting)
	{
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			throw ToolError(500, "SKSE TaskInterface unavailable");

		// Shared so the promise outlives a timed-out wait: if we return before the
		// task runs, the task can still set the (now-abandoned) promise safely.
		auto promise = std::make_shared<std::promise<json>>();
		auto future = promise->get_future();

		g_pendingTasks.fetch_add(1, std::memory_order_relaxed);
		task->AddTask([fn = std::move(a_fn), promise]() {
			// Stamp completion + drop the pending count on the main thread whether fn()
			// returns or throws — a routine ToolError must not leave the markers looking
			// like a stalled queue.
			struct Finally
			{
				~Finally()
				{
					g_lastTaskFrame.store(game::CurrentFrame(), std::memory_order_relaxed);
					g_pendingTasks.fetch_sub(1, std::memory_order_relaxed);
				}
			} finally;
			try {
				promise->set_value(fn());
			} catch (...) {
				try {
					promise->set_exception(std::current_exception());
				} catch (...) {
				}
			}
		});

		// Slice the wait so a caller that withdraws interest (a_keepWaiting clears) doesn't
		// block the full timeout — e.g. the recorder stopping while the main thread is stalled
		// mid-load. The abandoned task sets its shared promise safely once the main thread runs.
		const int      frameAtStart = game::CurrentFrame();
		constexpr auto kSlice = std::chrono::milliseconds(100);
		auto           remaining = a_timeout;
		auto           status = std::future_status::timeout;
		while (remaining.count() > 0) {
			if (a_keepWaiting && !a_keepWaiting->load(std::memory_order_relaxed))
				return json(nullptr);
			const auto slice = std::min(remaining, kSlice);
			status = future.wait_for(slice);
			if (status == std::future_status::ready)
				break;
			remaining -= slice;
		}
		if (status != std::future_status::ready) {
			// The engine's frame counter discriminates the two 504 causes: still
			// advancing = main thread busy (retry can succeed); frozen = hung OR
			// fully paused (the counter also freezes in pause menus).
			const int frameNow = game::CurrentFrame();
			if (frameAtStart < 0 || frameNow < 0)
				throw ToolError(504, std::format("main-thread task did not run within {}ms", a_timeout.count()));
			if (frameNow == frameAtStart)
				throw ToolError(504, std::format(
										 "main-thread task did not run within {}ms and the game frame counter has not advanced -- main thread hung or the game is fully paused; if no pause menu is open, only a process restart recovers",
										 a_timeout.count()));
			throw ToolError(504, std::format(
									 "main-thread task did not run within {}ms ({} frames elapsed -- main thread busy, a retry may succeed)",
									 a_timeout.count(), frameNow - frameAtStart));
		}

		return future.get();  // rethrows the handler's exception on the listener thread
	}

	int LastCompletedFrame()
	{
		return g_lastTaskFrame.load(std::memory_order_relaxed);
	}

	int PendingTasks()
	{
		return g_pendingTasks.load(std::memory_order_relaxed);
	}
}
