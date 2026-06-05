#pragma once

#include "Json.h"

namespace dvb
{
	struct ToolContext;

	namespace Papyrus
	{
		/// papyrus tool: list/describe the live Papyrus callable surface and invoke
		/// existing global (static) functions, returning the result value.
		///
		/// - action='list'     → loaded script class names (filter/limit)
		/// - action='describe' → one class's functions (params, return types) + properties
		/// - action='call'     → DispatchStaticCall(script, function, args) and return the value
		///
		/// Unlike console `cgf`, 'call' hands the return value back. Scalars and forms are
		/// supported both directions; array params/returns are not (yet). See Papyrus.cpp.
		json Handle(const json& a_args, const ToolContext& a_ctx);
	}
}
