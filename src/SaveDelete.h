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

	// Deletes a save through the engine's own entry-delete (the one the load menu's Delete button
	// uses), which removes the .ess and drops the entry from the manager's list. Main thread only.
	// a_name is the save's file stem, with or without ".ess". SE and AE only.
	Result Delete(const std::string& a_name);
}
