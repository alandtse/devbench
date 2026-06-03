#pragma once

// Test-only precompiled header. The production PCH (src/pch.h) pulls in
// RE/Skyrim.h + SKSE and aliases `namespace logs = SKSE::log`. The unit tests
// link NONE of CommonLibSSE-NG/SKSE — they compile only the host-independent
// production TUs (e.g. ToolRegistry.cpp) — so we supply a no-op `logs` stub with
// the same call surface. Messages are discarded; tests assert on return values.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINSOCKAPI_

#include <nlohmann/json.hpp>

#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace logs
{
	template <class... Args>
	void trace(Args&&...) noexcept
	{}
	template <class... Args>
	void debug(Args&&...) noexcept
	{}
	template <class... Args>
	void info(Args&&...) noexcept
	{}
	template <class... Args>
	void warn(Args&&...) noexcept
	{}
	template <class... Args>
	void error(Args&&...) noexcept
	{}
	template <class... Args>
	void critical(Args&&...) noexcept
	{}
}

using namespace std::literals;
