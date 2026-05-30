#include "MainThread.h"

#include "ToolRegistry.h"  // dvb::ToolError

#include <future>
#include <memory>

namespace dvb::MainThread
{
	json RunAndWait(std::function<json()> a_fn, std::chrono::milliseconds a_timeout)
	{
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			throw ToolError(500, "SKSE TaskInterface unavailable");

		// Shared so the promise outlives a timed-out wait: if we return before the
		// task runs, the task can still set the (now-abandoned) promise safely.
		auto promise = std::make_shared<std::promise<json>>();
		auto future = promise->get_future();

		task->AddTask([fn = std::move(a_fn), promise]() {
			try {
				promise->set_value(fn());
			} catch (...) {
				try {
					promise->set_exception(std::current_exception());
				} catch (...) {
				}
			}
		});

		if (future.wait_for(a_timeout) != std::future_status::ready)
			throw ToolError(504, std::format("main-thread task did not run within {}ms", a_timeout.count()));

		return future.get();  // rethrows the handler's exception on the listener thread
	}
}
