#pragma once

#include <chrono>
#include <functional>

#include "Json.h"

namespace dvb::MainThread
{
	/// Run `a_fn` on the main game thread (via SKSE's TaskInterface) and block the
	/// calling (listener) thread until it returns, propagating its JSON result — or
	/// rethrowing whatever it threw (e.g. a dvb::ToolError) on the caller's thread.
	///
	/// This is the value-returning primitive that lets tools read live game/render
	/// state synchronously (the agentic-renderdoc "Eval returns a value" model)
	/// rather than fire-and-forget + poll. Throws dvb::ToolError(504) if the task
	/// does not run within `a_timeout` (e.g. the main thread is stalled mid-load).
	///
	/// MUST be called from a non-main thread (the server listener). Calling it on the
	/// main thread would deadlock — the task can never run while this blocks.
	json RunAndWait(std::function<json()> a_fn,
		std::chrono::milliseconds         a_timeout = std::chrono::milliseconds(5000));
}
