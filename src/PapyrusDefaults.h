#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace dvb::PapyrusDefaults
{
	/// A default a Papyrus source file declares for an optional parameter. The compiler writes these
	/// into each call site, so the running VM never sees them; this is the table of the non-neutral
	/// ones (anything other than None / 0 / 0.0 / false / "") for every native function in the
	/// vanilla and SKSE sources, and the commonly called non-native ones.
	struct Value
	{
		enum class Kind
		{
			kBool,
			kInt,
			kFloat,
		};
		Kind             kind;
		double           number;
		std::string_view source;  ///< who ships the .psc signature the value is copied from: "vanilla" or "SKSE"
	};

	/// Matches the declaring script and function names and the parameter's 0-based position, ignoring case.
	/// A named parameter must also match by name; a native function's parameters have no names at run time
	/// ("param1", "param2", ...) and match by position alone. Anything else is not in the table.
	std::optional<Value> Find(std::string_view a_script, std::string_view a_function, std::string_view a_param,
		std::uint32_t a_index);

	/// What an omitted argument gets: the table's value only when its kind matches the parameter's
	/// type; else the type's neutral value only when the caller allowed it; else the call is refused.
	enum class Fill
	{
		kTable,
		kNeutral,
		kRefuse,
	};
	enum class ParamType
	{
		kBool,
		kInt,
		kFloat,
		kOther,
	};
	Fill Choose(const std::optional<Value>& a_table, ParamType a_type, bool a_allowNeutral);
}
