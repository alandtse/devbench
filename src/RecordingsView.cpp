#include "RecordingsView.h"

#include <algorithm>
#include <cctype>

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
}
