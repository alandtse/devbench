#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Json.h"

// Imgui-free projection of the `recordings` list into display rows, shared by both PCH-free menu
// TUs (SMF cimgui + FUCK real-imgui can't share a TU, but both link this from the main target).
namespace dvb::ui
{
	struct Row
	{
		std::string file, name, where, startText, timeText, format;
		long long   recordedMs = 0;  // numeric key for the Time column (timeText is display-only)
		bool        validated = false;
		bool        restorable = true;  // entry.kind != "unknown"
	};

	enum class SortKey
	{
		Name,
		Where,
		Start,
		Time,
		Format,
		Validated
	};

	// Project list["recordings"] into filtered + sorted rows. `filter` is a case-insensitive
	// substring over name/where/start (empty = all). Entries carrying an "error" are skipped.
	std::vector<Row> BuildRows(const json& a_list, const std::string& a_filter, SortKey a_key, bool a_ascending);

	std::string FormatDuration(long long a_ms);    // 91000 -> "1m31s"; < 1s -> "0s"
	std::string FormatStart(const json& a_entry);  // "save: X" / "coc: X" / "no restore point"
	std::string FormatWhere(const json& a_rec);    // worldspace else cell; " (interior)" suffix
	std::string KeyName(int a_dxScanCode);         // DXScanCode -> "F1"/"A"/… ; 0 -> "(unset)"

	// Menu action helpers (imgui-free — shared by both menu TUs). Each drives a devbench tool and
	// returns the tool's "error" message, or "" on success, so the caller can surface a failure.
	std::string Replay(const std::string& a_path);  // async record replay (empty path = most recent)
	std::string Validate(const std::string& a_file, bool a_value, bool a_invalidate = true);
	std::string Delete(const std::string& a_file, bool a_invalidate = true);
}
