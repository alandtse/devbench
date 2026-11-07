#pragma once

#include <string>

namespace dvb::SaveDelete
{
	enum class Result
	{
		kDeleted,
		kNotInList,
		kUnsupported
	};

	/// The save's file stem: a_name without a trailing ".ess" (any case).
	[[nodiscard]] std::string Stem(std::string a_name);

	// Deletes a save via the engine's load-menu Delete (removes the .ess and the list entry).
	// Main thread only; a_name is the file stem, with or without ".ess". SE and AE only.
	Result Delete(const std::string& a_name);
}
