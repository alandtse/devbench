#pragma once

#include "Json.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <utility>

namespace dvb::MainThread
{
	// The listener and game thread race to abandon or start a queued invocation.
	// Only a task that has not started can be abandoned; running code is never
	// interrupted. No caller-owned cancellation pointer escapes into the queue.
	class QueuedTask
	{
	public:
		using Clock = std::chrono::steady_clock;

		QueuedTask(std::function<json()> a_fn, Clock::time_point a_deadline) :
			_fn(std::move(a_fn)), _deadline(a_deadline)
		{}

		std::future<json> GetFuture() { return _promise.get_future(); }

		// True also covers a task the game thread already abandoned at its deadline.
		bool Abandon()
		{
			auto expected = State::kQueued;
			return _state.compare_exchange_strong(expected, State::kAbandoned) || expected == State::kAbandoned;
		}

		void Run(Clock::time_point a_now = Clock::now())
		{
			// Enforce the deadline here too: the listener may be descheduled when it
			// expires, and must not rely on waking first to cancel a stale mutation.
			if (a_now >= _deadline) {
				Abandon();
				return;
			}
			auto expected = State::kQueued;
			if (!_state.compare_exchange_strong(expected, State::kRunning))
				return;
			try {
				_promise.set_value(_fn());
			} catch (...) {
				try {
					_promise.set_exception(std::current_exception());
				} catch (...) {
				}
			}
		}

	private:
		enum class State
		{
			kQueued,
			kRunning,
			kAbandoned
		};

		std::function<json()> _fn;
		Clock::time_point     _deadline;
		std::promise<json>    _promise;
		std::atomic<State>    _state{ State::kQueued };
	};
}
