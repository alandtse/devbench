#pragma once

#include "Json.h"

namespace dvb
{
	struct ToolContext;

	namespace Papyrus
	{
		/// papyrus tool: list/describe the live Papyrus callable surface and invoke functions,
		/// returning the result value.
		///
		/// - action='list'     → loaded script class names (filter/limit)
		/// - action='describe' → one class's functions (params, return types) + properties
		/// - action='call'     → invoke a global (DispatchStaticCall) or a member function on a
		///                       form (`self`, DispatchMethodCall) and return the value
		///
		/// Unlike console `cgf`, 'call' hands the return value back. Scalars, forms, and scalar
		/// arrays are supported both directions. See Papyrus.cpp.
		json Handle(const json& a_args, const ToolContext& a_ctx);
	}
}
