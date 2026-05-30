#pragma once

#define WIN32_LEAN_AND_MEAN  // Exclude rarely-used stuff from Windows headers
#define NOMINMAX

// cpp-mcp's vendored cpp-httplib pulls in <winsock2.h>. CommonLib's transitive
// <Windows.h> would otherwise include the legacy <winsock.h>, which conflicts
// (sockaddr / WSAData redefinitions). Defining this first makes winsock2 the
// only one in the build. Mirrors Community Shaders' cpp-mcp integration.
#define _WINSOCKAPI_

#include <RE/Skyrim.h>
#include <REX/REX.h>
#include <SKSE/SKSE.h>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace logs = SKSE::log;
using namespace std::literals;
