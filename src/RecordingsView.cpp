#include "RecordingsView.h"

#include "Server.h"  // dvb::RunTool / InvalidateRecordingsCache (imgui-free)

#include <algorithm>
#include <cctype>
#include <utility>

namespace dvb::ui
{
	namespace
	{
		std::string Lower(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}
	}

	std::string FormatDuration(long long a_ms)
	{
		const long long s = a_ms / 1000;
		if (s <= 0)
			return "0s";
		const long long m = s / 60;
		return m > 0 ? std::to_string(m) + "m" + std::to_string(s % 60) + "s" : std::to_string(s) + "s";
	}

	std::string FormatStart(const json& a_entry)
	{
		const std::string kind = a_entry.value("kind", std::string{});
		if (kind == "save" || kind == "coc")
			return kind + ": " + a_entry.value("value", std::string{});
		return "no restore point";
	}

	std::string FormatWhere(const json& a_rec)
	{
		std::string where = a_rec.value("worldspace", std::string{});
		if (where.empty())
			where = a_rec.value("cell", std::string{});
		if (a_rec.value("interior", false))
			where += " (interior)";
		return where;
	}

	std::vector<Row> BuildRows(const json& a_list, const std::string& a_filter, SortKey a_key, bool a_ascending)
	{
		std::vector<Row>  rows;
		const std::string needle = Lower(a_filter);
		if (a_list.contains("recordings") && a_list["recordings"].is_array()) {
			for (const auto& r : a_list["recordings"]) {
				if (r.contains("error"))
					continue;
				try {
					// A hand-edited recording with a wrong-type meta field would make .value() throw
					// on the render thread — skip the bad entry rather than crash the menu.
					Row row;
					row.file = r.value("file", std::string{});
					row.name = r.value("name", row.file);
					row.format = r.value("format", std::string{ "?" });
					row.validated = r.value("validated", false);
					row.recordedMs = r.value("recordedMs", 0LL);
					row.timeText = FormatDuration(row.recordedMs);
					row.where = FormatWhere(r);
					const json entry = r.value("entry", json::object());
					row.startText = FormatStart(entry);
					row.restorable = entry.value("kind", std::string{}) != "unknown";
					if (!needle.empty() &&
						Lower(row.name + "\n" + row.where + "\n" + row.startText).find(needle) == std::string::npos)
						continue;
					rows.push_back(std::move(row));
				} catch (const std::exception&) {
					continue;
				}
			}
		}

		const auto less = [a_key](const Row& x, const Row& y) {
			switch (a_key) {
			case SortKey::Where:
				return x.where < y.where;
			case SortKey::Start:
				return x.startText < y.startText;
			case SortKey::Time:
				return x.recordedMs < y.recordedMs;
			case SortKey::Format:
				return x.format < y.format;
			case SortKey::Validated:
				return x.validated < y.validated;
			case SortKey::Name:
			default:
				return x.name < y.name;
			}
		};
		std::stable_sort(rows.begin(), rows.end(),
			[&](const Row& x, const Row& y) { return a_ascending ? less(x, y) : less(y, x); });
		return rows;
	}

	std::string Replay(const std::string& a_path)
	{
		json args{ { "action", "replay" }, { "restoreScene", true }, { "force", true } };
		if (!a_path.empty())
			args["path"] = a_path;
		return dvb::RunTool("record", args).value("error", std::string{});
	}

	std::string Validate(const std::string& a_file, bool a_value, bool a_invalidate)
	{
		const json r = dvb::RunTool("recordings", json{ { "action", "validate" }, { "file", a_file }, { "value", a_value } });
		if (a_invalidate)
			dvb::InvalidateRecordingsCache();
		return r.value("error", std::string{});
	}

	std::string Delete(const std::string& a_file, bool a_invalidate)
	{
		const json r = dvb::RunTool("recordings", json{ { "action", "delete" }, { "file", a_file } });
		if (a_invalidate)
			dvb::InvalidateRecordingsCache();
		return r.value("error", std::string{});
	}

	std::string KeyName(int a_code)
	{
		if (a_code == 0)
			return "(unset)";
		switch (a_code) {
		case 1:
			return "Esc";
		case 14:
			return "Backspace";
		case 15:
			return "Tab";
		case 28:
			return "Enter";
		case 29:
			return "LCtrl";
		case 42:
			return "LShift";
		case 54:
			return "RShift";
		case 56:
			return "LAlt";
		case 57:
			return "Space";
		case 58:
			return "CapsLock";
		case 157:
			return "RCtrl";
		case 184:
			return "RAlt";
		case 199:
			return "Home";
		case 200:
			return "Up";
		case 201:
			return "PageUp";
		case 203:
			return "Left";
		case 205:
			return "Right";
		case 207:
			return "End";
		case 208:
			return "Down";
		case 209:
			return "PageDown";
		case 210:
			return "Insert";
		case 211:
			return "Delete";
		default:
			break;
		}
		if (a_code >= 2 && a_code <= 10)  // top-row 1..9
			return std::string(1, static_cast<char>('1' + (a_code - 2)));
		if (a_code == 11)
			return "0";
		if (a_code >= 59 && a_code <= 68)  // F1..F10
			return "F" + std::to_string(a_code - 58);
		if (a_code == 87)
			return "F11";
		if (a_code == 88)
			return "F12";
		// Letters, in DirectInput scancode layout order (not alphabetical).
		static const std::pair<int, const char*> letters[] = {
			{ 16, "Q" }, { 17, "W" }, { 18, "E" }, { 19, "R" }, { 20, "T" }, { 21, "Y" }, { 22, "U" }, { 23, "I" }, { 24, "O" }, { 25, "P" },
			{ 30, "A" }, { 31, "S" }, { 32, "D" }, { 33, "F" }, { 34, "G" }, { 35, "H" }, { 36, "J" }, { 37, "K" }, { 38, "L" },
			{ 44, "Z" }, { 45, "X" }, { 46, "C" }, { 47, "V" }, { 48, "B" }, { 49, "N" }, { 50, "M" }
		};
		for (const auto& [code, name] : letters)
			if (code == a_code)
				return name;
		return "key " + std::to_string(a_code);
	}
}
