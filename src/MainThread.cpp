#include "MainThread.h"

#include "GameState.h"  // dvb::game::CurrentFrame
#include "MainThreadTask.h"
#include "MainThreadTimeout.h"
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
	json RunAndWait(std::function<json()> a_fn, std::chrono::milliseconds a_timeout, const std::atomic<bool>* a_keepWaiting, Queue a_queue)
	{
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			throw ToolError(500, "SKSE TaskInterface unavailable");
		if (a_keepWaiting && !a_keepWaiting->load(std::memory_order_relaxed))
			return json(nullptr);

		const auto deadline = QueuedTask::Clock::now() + a_timeout;
		auto       invocation = std::make_shared<QueuedTask>(std::move(a_fn), deadline);
		auto       future = invocation->GetFuture();

		const bool countsTowardHealth = a_queue == Queue::kMain;
		if (countsTowardHealth)
			g_pendingTasks.fetch_add(1, std::memory_order_relaxed);
		auto run = [invocation, countsTowardHealth]() {
			// Stamp completion + drop the pending count on the main thread whether fn()
			// returns or throws — a routine ToolError must not leave the markers looking
			// like a stalled queue.
			struct Finally
			{
				bool countsTowardHealth;
				~Finally()
				{
					if (!countsTowardHealth)
						return;
					g_lastTaskFrame.store(game::CurrentFrame(), std::memory_order_relaxed);
					g_pendingTasks.fetch_sub(1, std::memory_order_relaxed);
				}
			} finally{ countsTowardHealth };
			invocation->Run();
		};
		if (a_queue == Queue::kUi)
			task->AddUITask(std::move(run));
		else
			task->AddTask(std::move(run));

		// Slice the wait so a caller that withdraws interest (a_keepWaiting clears) doesn't
		// block the full timeout — e.g. the recorder stopping while the main thread is stalled
		// mid-load. Abandon prevents a queued mutation from running after this call returns;
		// a mutation that already started cannot be interrupted.
		const int      frameAtStart = game::CurrentFrame();
		constexpr auto kSlice = std::chrono::milliseconds(100);
		while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
			if (a_keepWaiting && !a_keepWaiting->load(std::memory_order_relaxed)) {
				invocation->Abandon();
				if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
					return future.get();
				return json(nullptr);
			}
			const auto now = QueuedTask::Clock::now();
			if (now >= deadline)
				break;
			future.wait_until(std::min(deadline, now + kSlice));
		}
		if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
			const bool abandoned = invocation->Abandon();
			if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
				return future.get();
			if (!abandoned)
				throw TaskTimeout(true, DescribeTaskTimeout(a_timeout, true));
			throw TaskTimeout(false, DescribeTaskTimeout(a_timeout, false, frameAtStart, game::CurrentFrame()));
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
