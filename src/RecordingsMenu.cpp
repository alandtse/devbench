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

#include "InputHotkeys.h"    // dvb::GetHotkeys (read-only keybinds display)
#include "Json.h"            // dvb::json (nlohmann) — unrelated to imgui, safe in this TU
#include "RecordingsView.h"  // dvb::ui row model
#include "Server.h"          // dvb::RunTool / OpenRecordingsFolder

#include <set>
#include <string>

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
		bool                  s_confirmDelete = false;
		ui::SortKey           s_sortKey = ui::SortKey::Name;
		bool                  s_sortAsc = true;

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

		// Replay a recording (or, with an empty path, the most recent). ASYNC by default so the
		// call returns immediately — a blocking replay here would stall the render thread.
		void ReplayPath(const std::string& a_path)
		{
			json args{ { "action", "replay" }, { "restoreScene", true }, { "force", true } };
			if (!a_path.empty())
				args["path"] = a_path;
			dvb::RunTool("record", args);
		}

		void Validate(const std::string& a_file, bool a_value)
		{
			dvb::RunTool("recordings", json{ { "action", "validate" }, { "file", a_file }, { "value", a_value } });
			dvb::InvalidateRecordingsCache();
		}

		void Delete(const std::string& a_file)
		{
			dvb::RunTool("recordings", json{ { "action", "delete" }, { "file", a_file } });
			dvb::InvalidateRecordingsCache();
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
					ReplayPath(a_dir + "/" + file);
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button((std::string(validated ? "Unvalidate" : "Validate") + id).c_str()))
					Validate(file, !validated);
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button((std::string("Delete") + id).c_str()))
					Delete(file);
				ImGuiMCP::Separator();
			}
		}

		// Bulk action bar over the current selection; a delete needs a second (confirm) click.
		void RenderActionBar(const json& a_list)
		{
			ImGuiMCP::BeginDisabled(s_selected.empty());
			if (ImGuiMCP::Button("Validate selected"))
				for (const auto& f : s_selected)
					Validate(f, true);
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Unvalidate selected"))
				for (const auto& f : s_selected)
					Validate(f, false);
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Delete selected"))
				s_confirmDelete = true;
			ImGuiMCP::EndDisabled();
			if (s_confirmDelete && !s_selected.empty()) {
				ImGuiMCP::SameLine();
				ImGuiMCP::TextColored(kRed, "delete %d?", static_cast<int>(s_selected.size()));
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button("Yes##confdel")) {
					for (const auto& f : s_selected)
						Delete(f);
					s_selected.clear();
					s_confirmDelete = false;
				}
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button("No##confdel"))
					s_confirmDelete = false;
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Open folder"))
				dvb::OpenRecordingsFolder();
			(void)a_list;
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
			case 5:
				return ui::SortKey::Format;
			case 6:
				return ui::SortKey::Validated;
			default:
				return ui::SortKey::Name;
			}
		}

		void RenderRecordingsTable(const json& a_list, const std::string& a_dir)
		{
			static const bool s_filterOk = FilterExportOk();
			if (s_filterOk)
				ImGuiMCP::InputTextWithHint("##filter", "filter name / where / start", s_filter, sizeof s_filter);
			else
				ImGuiMCP::TextColored(kGrey, "(filter unavailable on this menu-framework build)");

			RenderActionBar(a_list);

			constexpr ImGuiMCP::ImGuiTableFlags flags = ImGuiMCP::ImGuiTableFlags_Resizable |
			                                            ImGuiMCP::ImGuiTableFlags_Sortable |
			                                            ImGuiMCP::ImGuiTableFlags_RowBg |
			                                            ImGuiMCP::ImGuiTableFlags_ScrollY |
			                                            ImGuiMCP::ImGuiTableFlags_BordersInnerH;
			// Fill the rest of the page rather than a fixed 320px box (ScrollY needs a height).
			ImGuiMCP::ImVec2 avail{ 0.0f, 0.0f };
			ImGuiMCP::GetContentRegionAvail(&avail);
			if (!ImGuiMCP::BeginTable("recs", 8, flags, ImGuiMCP::ImVec2(0.0f, avail.y)))
				return;
			ImGuiMCP::TableSetupColumn("##sel", ImGuiMCP::ImGuiTableColumnFlags_NoSort, 0.0f, 0);
			ImGuiMCP::TableSetupColumn("Name", ImGuiMCP::ImGuiTableColumnFlags_DefaultSort, 0.0f, 1);
			ImGuiMCP::TableSetupColumn("Where", 0, 0.0f, 2);
			ImGuiMCP::TableSetupColumn("Start", 0, 0.0f, 3);
			ImGuiMCP::TableSetupColumn("Time", 0, 0.0f, 4);
			ImGuiMCP::TableSetupColumn("Fmt", 0, 0.0f, 5);
			ImGuiMCP::TableSetupColumn("Val", 0, 0.0f, 6);
			ImGuiMCP::TableSetupColumn("Actions", ImGuiMCP::ImGuiTableColumnFlags_NoSort, 0.0f, 7);
			ImGuiMCP::TableSetupScrollFreeze(0, 1);
			ImGuiMCP::TableHeadersRow();

			if (ImGuiMCP::ImGuiTableSortSpecs* specs = ImGuiMCP::TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
				s_sortKey = ColumnToSort(specs->Specs[0].ColumnUserID);
				s_sortAsc = specs->Specs[0].SortDirection != ImGuiMCP::ImGuiSortDirection_Descending;
			}

			for (const auto& row : ui::BuildRows(a_list, s_filter, s_sortKey, s_sortAsc)) {
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
				ImGuiMCP::TableNextColumn();
				ImGuiMCP::Text("%s", row.where.c_str());
				ImGuiMCP::TableNextColumn();
				if (row.restorable)
					ImGuiMCP::Text("%s", row.startText.c_str());
				else
					ImGuiMCP::TextColored(kRed, "%s", row.startText.c_str());
				ImGuiMCP::TableNextColumn();
				ImGuiMCP::Text("%s", row.timeText.c_str());
				ImGuiMCP::TableNextColumn();
				ImGuiMCP::Text("%s", row.format.c_str());
				ImGuiMCP::TableNextColumn();
				ImGuiMCP::TextColored(row.validated ? kGreen : kGrey, row.validated ? "yes" : "no");
				ImGuiMCP::TableNextColumn();
				const std::string id = "##" + row.file;
				if (ImGuiMCP::Button(("Replay" + id).c_str()))
					ReplayPath(a_dir + "/" + row.file);
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button(((row.validated ? "Unval" : "Val") + id).c_str()))
					Validate(row.file, !row.validated);
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button(("Del" + id).c_str())) {
					Delete(row.file);
					s_selected.erase(row.file);
				}
			}
			ImGuiMCP::EndTable();
		}

		void __stdcall RenderRecordings()
		{
			ImGuiMCP::TextWrapped(
				"Replay a captured trajectory against the current build. Replay is "
				"async; it restores the recorded scene first (force = run anyway on a mismatch).");
			ImGuiMCP::Spacing();
			if (ImGuiMCP::Button("Replay last"))
				ReplayPath("");
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

		void __stdcall RenderKeybinds()
		{
			int  recKey = 0, repKey = 0;
			bool recShift = false, repShift = false;
			dvb::GetHotkeys(recKey, recShift, repKey, repShift);
			ImGuiMCP::Text("recordHotkey : %d%s", recKey, recShift ? " +Shift" : "");
			ImGuiMCP::Text("replayHotkey : %d%s", repKey, repShift ? " +Shift" : "");
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
		SKSEMenuFramework::AddSectionItem("Keybinds", RenderKeybinds);
		logger::info("devbench: registered SMF pages (framework v{:.1f}).", SKSEMenuFramework::GetMenuFrameworkVersion());
	}
}
