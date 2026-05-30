#pragma once

// Single JSON type used across the registry, adapters, and handlers. We pull in
// nlohmann_json directly (the same library cpp-mcp uses); cpp-mcp's mcp_message.h
// is patched to include this same <nlohmann/json.hpp> so there is exactly one
// ABI-tagged namespace across our code and cpp-mcp's — no LNK2001 from a split.
#include <nlohmann/json.hpp>

namespace dvb
{
	using json = nlohmann::json;
}
