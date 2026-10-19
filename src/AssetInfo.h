#pragma once

#include "Json.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace dvb::AssetInfo
{
	/// Main thread. The TES4 header of a loaded plugin read from Data\<name>:
	/// { name, author, description, version, recordCount, masters, master, light }. Null when the name is not a plain file
	/// name or the file cannot be read as a plugin.
	json Plugin(std::string_view a_name);

	/// Main thread. Every loaded plugin (full then light), described as Plugin does, with its load-order index; a_filter is a
	/// case-insensitive substring of the name, author or description.
	json Plugins(const std::string& a_filter, std::size_t a_limit, std::size_t a_offset);

	/// Main thread. A game file (a path relative to Meshes or to the data root) read from the archives or loose files:
	/// { path, found, resolvedPath, bytes } and, for a Havok animation, className, durationSeconds, transformTracks and
	/// floatTracks. found is false when no file by that path is readable.
	json Animation(const std::string& a_path);

	/// Main thread. The animation an idle plays, from the file its record names (a_file) and its event: when that file is an
	/// animation, its Animation description with source "file"; when it is a behavior graph, the clip the event plays, found
	/// through the graph and the behavior files it references, with source "behavior" and { behavior, clip, animationName }
	/// beside the Animation description of the clip's file. A graph names its clip after the event, so a clip that is not
	/// named for its event is not found (found false, with a note).
	json IdleAnimation(const std::string& a_file, const std::string& a_event);
}
