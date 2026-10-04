#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace dvb::PapyrusDefaults
{
	/// A default a Papyrus source file declares for an optional parameter. The compiler writes these
	/// into each call site, so the running VM never sees them; this is the table of the non-neutral
	/// ones (anything other than None / 0 / 0.0 / false / "") for every native function in the vanilla and SKSE
	/// sources and the commonly called non-native ones.
	struct Value
	{
		enum class Kind
		{
			kBool,
			kInt,
			kFloat,
		};
		Kind   kind;
		double number;
	};

	/// Matches the declaring script, function, and parameter names, ignoring case. A native function's parameters
	/// have no names at run time ("param1", "param2", ...); pass the 0-based position and those match by position.
	std::optional<Value> Find(std::string_view a_script, std::string_view a_function, std::string_view a_param,
		std::optional<std::uint32_t> a_index = std::nullopt);
}
