#pragma once

#include "Json.h"
#include "ToolRegistry.h"  // ToolContext, ToolDescriptor

namespace dvb::Map
{
	/// The `map` tool: read, set or clear the player's custom map marker, and look up a Location's world marker.
	ToolDescriptor BuildDescriptor();
	json           Handle(const json& a_args, const ToolContext& a_ctx);
}
