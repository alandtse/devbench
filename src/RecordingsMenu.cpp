// devbench's optional in-game menu, hosted by SKSE Menu Framework 3. PCH-FREE and must stay so: the
// SMF header's cimgui ImGuiMCP typedefs can't coexist with the real imgui.h in the main PCH. All
// drawing goes through ImGuiMCP (host context via GetProcAddress); inert when SMF is absent
// (SKSEMenuFramework::IsInstalled() gates registration).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "RecordingsMenu.h"

#include <RE/Skyrim.h>  // RE::InputEvent — referenced by SMF header callback typedefs
#include <SKSE/SKSE.h>  // SKSE::log (this TU has no PCH `logs` alias)

#include "SKSEMenuFramework.h"

#include "InputHotkeys.h"    // dvb::GetHotkeys (read-only hotkey display)
#include "Json.h"            // dvb::json (nlohmann) — unrelated to imgui, safe in this TU
#include "RecordingsView.h"  // dvb::ui row model
#include "Server.h"          // dvb::RunTool / OpenRecordingsFolder

#include <set>
#include <string>
#include <vector>

namespace logger = SKSE::log;

namespace dvb::UI
{
	namespace
	{
		constexpr ImGuiMCP::ImVec4 kGrey{ 0.7f, 0.7f, 0.7f, 1.0f };
		constexpr ImGuiMCP::ImVec4 kGreen{ 0.40f, 0.85f, 0.45f, 1.0f };
		constexpr ImGuiMCP::ImVec4 kRed{ 1.0f, 0.45f, 0.45f, 1.0f };

		// Menu state (single SMF page → function-local statics are fine).
		char                  s_filter[128]{};
		std::set<std::string> s_selected;
		bool                  s_confirmBulkDelete = false;
		std::string           s_confirmDeleteFile;  // the row awaiting a delete confirm
		ui::SortKey           s_sortKey = ui::SortKey::Name;
		bool                  s_sortAsc = true;
		std::string           s_lastError, s_lastStatus;  // last action outcome (red / green)

		void Tooltip(const char* a_text)
		{
			if (ImGuiMCP::IsItemHovered())
				ImGuiMCP::SetTooltip("%s", a_text);
		}

		void SetStatus(const std::string& a_s)
		{
			s_lastStatus = a_s;
			s_lastError.clear();
		}
		void SetError(const std::string& a_e)
		{
			s_lastError = a_e;
			s_lastStatus.clear();
		}

		// Action wrappers over the shared imgui-free helpers — each sets the status/error line.
		void ReplayAction(const std::string& a_path)
		{
			if (const std::string e = ui::Replay(a_path); e.empty())
				SetStatus("Replay started");
			else
				SetError("Replay failed: " + e);
		}
		void ValidateAction(const std::string& a_file, bool a_value)
		{
			if (const std::string e = ui::Validate(a_file, a_value); e.empty())
				SetStatus((a_value ? "Validated " : "Unvalidated ") + a_file);
			else
				SetError("Validate failed: " + e);
		}
		void DeleteAction(const std::string& a_file)
		{
			if (const std::string e = ui::Delete(a_file); e.empty())
				SetStatus("Deleted " + a_file);
			else
				SetError("Delete failed: " + e);
		}
		void ReportBulk(int a_fail, int a_total, const char* a_verb, const std::string& a_first)
		{
			if (a_fail == 0)
				SetStatus(std::to_string(a_total) + " " + a_verb);
			else
				SetError(std::to_string(a_fail) + " of " + std::to_string(a_total) + " " + a_verb + " failed: " + a_first);
		}

		// The SMF host resolves each ImGuiMCP call via GetProcAddress; the table/sort exports may be
		// absent on an older host. Probe a fresh module handle (the header's cached one can be NULL
		// if devbench loaded first) once; fall back to the plain per-row layout if any are missing.
		bool ProbeTables()
		{
			HMODULE smf = ::GetModuleHandleW(L"SKSEMenuFramework");
			return smf &&
			       ::GetProcAddress(smf, "igBeginTable") &&
			       ::GetProcAddress(smf, "igTableSetupColumn") &&
			       ::GetProcAddress(smf, "igTableHeadersRow") &&
			       ::GetProcAddress(smf, "igTableGetSortSpecs") &&
			       ::GetProcAddress(smf, "igCheckbox");
		}

		bool FilterExportOk()
		{
			HMODULE smf = ::GetModuleHandleW(L"SKSEMenuFramework");
			return smf && ::GetProcAddress(smf, "igInputTextWithHint");
		}

		// Pre-table per-recording block layout, kept as the fallback when the host lacks the table
		// exports. (Was the whole of RenderRecordings before the table rework.)
		void RenderRecordingsLegacy(const json& a_recs, const std::string& a_dir)
		{
			int idx = 0;
			for (const auto& r : a_recs) {
				const std::string file = r.value("file", std::string{});
				const std::string name = r.value("name", file);
				const std::string fmt = r.value("format", std::string{ "?" });
				const bool        validated = r.value("validated", false);
				const int         samples = r.value("sampleCount", 0);
				const std::string id = "##rec" + std::to_string(idx++);

				ImGuiMCP::Text("%s", name.c_str());
				ImGuiMCP::TextColored(kGrey, "  %s | %d samples", fmt.c_str(), samples);
				ImGuiMCP::SameLine();
				ImGuiMCP::TextColored(validated ? kGreen : kGrey, validated ? "| validated" : "| unvalidated");

				if (ImGuiMCP::Button((std::string("Replay") + id).c_str()))
					ReplayAction(a_dir + "/" + file);
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button((std::string(validated ? "Unvalidate" : "Validate") + id).c_str()))
					ValidateAction(file, !validated);
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button((std::string("Delete") + id).c_str()))
					DeleteAction(file);
				ImGuiMCP::Separator();
			}
		}

		// Select-all + bulk action bar over the currently-filtered rows.
		void RenderActionBar(const std::vector<ui::Row>& a_rows)
		{
			if (ImGuiMCP::Button("All"))
				for (const auto& r : a_rows)
					s_selected.insert(r.file);
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("None"))
				s_selected.clear();
			ImGuiMCP::SameLine();
			ImGuiMCP::Text("(%d selected)", static_cast<int>(s_selected.size()));

			ImGuiMCP::BeginDisabled(s_selected.empty());
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Validate selected")) {
				int         fail = 0;
				std::string first;
				for (const auto& f : s_selected)
					if (const std::string e = ui::Validate(f, true, false); !e.empty() && (++fail, first.empty()))
						first = e;
				dvb::InvalidateRecordingsCache();
				ReportBulk(fail, static_cast<int>(s_selected.size()), "validated", first);
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Unvalidate selected")) {
				int         fail = 0;
				std::string first;
				for (const auto& f : s_selected)
					if (const std::string e = ui::Validate(f, false, false); !e.empty() && (++fail, first.empty()))
						first = e;
				dvb::InvalidateRecordingsCache();
				ReportBulk(fail, static_cast<int>(s_selected.size()), "unvalidated", first);
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Delete selected"))
				s_confirmBulkDelete = true;
			ImGuiMCP::EndDisabled();
			if (s_confirmBulkDelete && !s_selected.empty()) {
				ImGuiMCP::SameLine();
				ImGuiMCP::TextColored(kRed, "delete %d?", static_cast<int>(s_selected.size()));
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button("Yes##confdel")) {
					int         fail = 0;
					std::string first;
					for (const auto& f : s_selected)
						if (const std::string e = ui::Delete(f, false); !e.empty() && (++fail, first.empty()))
							first = e;
					dvb::InvalidateRecordingsCache();
					ReportBulk(fail, static_cast<int>(s_selected.size()), "deleted", first);
					s_selected.clear();
					s_confirmBulkDelete = false;
				}
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button("No##confdel"))
					s_confirmBulkDelete = false;
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Open folder"))
				dvb::OpenRecordingsFolder();
		}

		ui::SortKey ColumnToSort(unsigned a_userID)
		{
			switch (a_userID) {
			case 2:
				return ui::SortKey::Where;
			case 3:
				return ui::SortKey::Start;
			case 4:
				return ui::SortKey::Time;
			case 6:
				return ui::SortKey::Validated;
			default:
				return ui::SortKey::Name;
			}
		}

		void RenderRowActions(const ui::Row& a_row, const std::string& a_dir)
		{
			const std::string id = "##" + a_row.file;
			if (ImGuiMCP::Button(("Replay" + id).c_str()))
				ReplayAction(a_dir + "/" + a_row.file);
			Tooltip("Replay this recording.");
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button(((a_row.validated ? "Unvalidate" : "Validate") + id).c_str()))
				ValidateAction(a_row.file, !a_row.validated);
			Tooltip("Mark this recording validated / not.");
			ImGuiMCP::SameLine();
			// Per-row delete needs a confirm too (bulk delete already does).
			if (s_confirmDeleteFile == a_row.file) {
				if (ImGuiMCP::Button(("Yes" + id).c_str())) {
					DeleteAction(a_row.file);
					s_selected.erase(a_row.file);
					s_confirmDeleteFile.clear();
				}
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button(("No" + id).c_str()))
					s_confirmDeleteFile.clear();
			} else {
				if (ImGuiMCP::Button(("Delete" + id).c_str()))
					s_confirmDeleteFile = a_row.file;
				Tooltip("Delete this recording (asks to confirm).");
			}
		}

		void RenderRecordingsTable(const json& a_list, const std::string& a_dir)
		{
			static const bool s_filterOk = FilterExportOk();
			ImGuiMCP::Text("Filter");
			ImGuiMCP::SameLine();
			if (s_filterOk)
				ImGuiMCP::InputTextWithHint("##filter", "name / where / start", s_filter, sizeof s_filter);
			else
				ImGuiMCP::TextColored(kGrey, "(filter unavailable on this menu-framework build)");

			// One build per frame; the table reads sort specs to update s_sortKey for the next frame.
			const auto rows = ui::BuildRows(a_list, s_filter, s_sortKey, s_sortAsc);
			RenderActionBar(rows);

			constexpr ImGuiMCP::ImGuiTableFlags flags = ImGuiMCP::ImGuiTableFlags_Resizable |
			                                            ImGuiMCP::ImGuiTableFlags_Sortable |
			                                            ImGuiMCP::ImGuiTableFlags_RowBg |
			                                            ImGuiMCP::ImGuiTableFlags_ScrollY |
			                                            ImGuiMCP::ImGuiTableFlags_BordersInnerH;
			// Fill the rest of the page rather than a fixed box (ScrollY needs a height).
			ImGuiMCP::ImVec2 avail{ 0.0f, 0.0f };
			ImGuiMCP::GetContentRegionAvail(&avail);
			if (!ImGuiMCP::BeginTable("recs", 7, flags, ImGuiMCP::ImVec2(0.0f, avail.y)))
				return;
			ImGuiMCP::TableSetupColumn("Sel", ImGuiMCP::ImGuiTableColumnFlags_NoSort, 0.0f, 0);
			ImGuiMCP::TableSetupColumn("Name", ImGuiMCP::ImGuiTableColumnFlags_DefaultSort, 0.0f, 1);
			ImGuiMCP::TableSetupColumn("Where", 0, 0.0f, 2);
			ImGuiMCP::TableSetupColumn("Start", 0, 0.0f, 3);
			ImGuiMCP::TableSetupColumn("Time", 0, 0.0f, 4);
			ImGuiMCP::TableSetupColumn("Valid", 0, 0.0f, 6);
			ImGuiMCP::TableSetupColumn("Actions", ImGuiMCP::ImGuiTableColumnFlags_NoSort, 0.0f, 7);
			ImGuiMCP::TableSetupScrollFreeze(0, 1);
			ImGuiMCP::TableHeadersRow();

			if (ImGuiMCP::ImGuiTableSortSpecs* specs = ImGuiMCP::TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
				s_sortKey = ColumnToSort(specs->Specs[0].ColumnUserID);
				s_sortAsc = specs->Specs[0].SortDirection != ImGuiMCP::ImGuiSortDirection_Descending;
			}

			if (rows.empty()) {
				ImGuiMCP::TableNextRow();
				ImGuiMCP::TableNextColumn();
				if (s_filter[0])
					ImGuiMCP::TextColored(kGrey, "No matches for \"%s\"", s_filter);
				else {
					int  recKey = 0, repKey = 0;
					bool recShift = false, repShift = false;
					dvb::GetHotkeys(recKey, recShift, repKey, repShift);
					ImGuiMCP::TextColored(kGrey, "No recordings yet — press %s to record.", ui::KeyName(recKey).c_str());
				}
			}

			for (const auto& row : rows) {
				ImGuiMCP::TableNextRow();
				ImGuiMCP::TableNextColumn();
				bool sel = s_selected.count(row.file) > 0;
				if (ImGuiMCP::Checkbox(("##sel" + row.file).c_str(), &sel)) {
					if (sel)
						s_selected.insert(row.file);
					else
						s_selected.erase(row.file);
				}
				ImGuiMCP::TableNextColumn();
				ImGuiMCP::Text("%s", row.name.c_str());
				Tooltip(("format: " + row.format).c_str());
				ImGuiMCP::TableNextColumn();
				ImGuiMCP::Text("%s", row.where.c_str());
				ImGuiMCP::TableNextColumn();
				if (row.restorable)
					ImGuiMCP::Text("%s", row.startText.c_str());
				else {
					ImGuiMCP::TextColored(kRed, "%s", row.startText.c_str());
					Tooltip("No save/coc entry captured — replay can't restore the starting scene; comparisons may be unreliable.");
				}
				ImGuiMCP::TableNextColumn();
				ImGuiMCP::Text("%s", row.timeText.c_str());
				ImGuiMCP::TableNextColumn();
				ImGuiMCP::TextColored(row.validated ? kGreen : kGrey, row.validated ? "yes" : "no");
				ImGuiMCP::TableNextColumn();
				RenderRowActions(row, a_dir);
			}
			ImGuiMCP::EndTable();
		}

		void __stdcall RenderRecordings()
		{
			ImGuiMCP::TextWrapped(
				"Replay a captured trajectory against the current build. Replay is "
				"async; it restores the recorded scene first (force = run anyway on a mismatch).");
			ImGuiMCP::Spacing();
			if (ImGuiMCP::Button("Replay most recent"))
				ReplayAction("");
			Tooltip("Replay the most recently recorded trajectory (async).");
			if (!s_lastError.empty())
				ImGuiMCP::TextColored(kRed, "%s", s_lastError.c_str());
			else if (!s_lastStatus.empty())
				ImGuiMCP::TextColored(kGreen, "%s", s_lastStatus.c_str());
			ImGuiMCP::Separator();

			const json list = dvb::ListRecordingsCached();
			if (list.contains("error")) {
				ImGuiMCP::TextColored(kRed, "recordings unavailable: %s", list.value("error", std::string{}).c_str());
				return;
			}
			const std::string dir = list.value("dir", std::string{});

			static const bool s_tablesOk = ProbeTables();
			if (s_tablesOk)
				RenderRecordingsTable(list, dir);
			else
				RenderRecordingsLegacy(list.value("recordings", json::array()), dir);
		}

		void __stdcall RenderHotkeys()
		{
			int  recKey = 0, repKey = 0;
			bool recShift = false, repShift = false;
			dvb::GetHotkeys(recKey, recShift, repKey, repShift);
			ImGuiMCP::Text("Record : %s%s", ui::KeyName(recKey).c_str(), recShift ? " +Shift" : "");
			ImGuiMCP::Text("Replay : %s%s", ui::KeyName(repKey).c_str(), repShift ? " +Shift" : "");
			ImGuiMCP::Separator();
			ImGuiMCP::TextWrapped(
				"Rebind in the FUCK menu (if installed) or edit Data/SKSE/Plugins/devbench/config.json "
				"(DXScanCode integers; 0 = disabled). SMF has no rebind widget.");
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			logger::info("SKSE Menu Framework not installed; devbench in-game menu disabled.");
			return;
		}
		SKSEMenuFramework::SetSection("devbench");
		SKSEMenuFramework::AddSectionItem("Recordings", RenderRecordings);
		SKSEMenuFramework::AddSectionItem("Hotkeys", RenderHotkeys);
		logger::info("devbench: registered SMF pages (framework v{:.1f}).", SKSEMenuFramework::GetMenuFrameworkVersion());
	}
}
