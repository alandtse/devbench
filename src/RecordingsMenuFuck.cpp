// devbench's optional in-game menu, hosted by FUCK (a keybind + ImGui menu framework).
//
// This TU is PCH-FREE and must stay that way: FUCK_API.h pulls in the real imgui.h, which cannot
// coexist with SMF's cimgui ImGuiMCP in RecordingsMenu.cpp — hence a separate PCH-free target
// (devbench-UI-fuck). Drawing routes through the FUCK:: wrappers (the HOST's ImGui, via the
// interface fetched from the FUCK.dll "RequestFUCK" export), so the module is inert when FUCK is
// not installed (FUCK::Connect returns false and nothing is registered).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "RecordingsMenu.h"

#include <RE/Skyrim.h>  // RE::InputEvent — referenced by FUCK API callback typedefs
#include <SKSE/SKSE.h>  // SKSE::log (this TU has no PCH `logs` alias)

#include <SimpleIni.h>  // FUCK_API.h references CSimpleIniA in its INI/keybind callbacks — include first

#include "FUCK_API.h"  // pulls in <imgui.h> for types; drawing via FUCK:: wrappers

#include "InputHotkeys.h"    // dvb::SetRecordHotkey / GetHotkeys
#include "Json.h"            // dvb::json (nlohmann) — unrelated to imgui, safe in this TU
#include "RecordingsView.h"  // dvb::ui row model
#include "Server.h"          // dvb::RunTool / OpenRecordingsFolder

#include <cstdint>
#include <set>
#include <string>

namespace logger = SKSE::log;

namespace dvb::UI
{
	namespace
	{
		const ImVec4 kGrey{ 0.7f, 0.7f, 0.7f, 1.0f };
		const ImVec4 kGreen{ 0.40f, 0.85f, 0.45f, 1.0f };
		const ImVec4 kRed{ 1.0f, 0.45f, 0.45f, 1.0f };

		// Replay a recording (empty path → most recent). ASYNC by default so the call returns
		// immediately — a blocking replay here would stall the render thread.
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

		// Apply a completed rebind. FUCK owns the capture (DrawManagedHotkey + OnAsyncInput); once it
		// finishes (isBinding clears, kKey changed) push the new bind to the sink + config. Only Shift
		// and keyboard binds are supported, so a Ctrl/Alt or gamepad bind is rejected with a warning.
		void ApplyBind(FUCK::ManagedHotkey& a_h, std::uint32_t& a_last, bool a_isRecord, std::string& a_warn)
		{
			if (a_h.isBinding || a_h.kKey == a_last)
				return;
			a_last = a_h.kKey;
			if (a_h.kKey == 0) {
				a_warn = "keyboard only — bind ignored";
				return;
			}
			if (a_h.kMod2 != -1 || (a_h.kMod1 != -1 && a_h.kMod1 != static_cast<int>(FUCK::Modifier::kShift))) {
				a_warn = "only Shift modifier supported";
				return;
			}
			a_warn.clear();
			const bool shift = a_h.kMod1 == static_cast<int>(FUCK::Modifier::kShift);
			if (a_isRecord)
				dvb::SetRecordHotkey(static_cast<int>(a_h.kKey), shift);
			else
				dvb::SetReplayHotkey(static_cast<int>(a_h.kKey), shift);
		}

		// A FUCK sidebar tool (grouped under "devbench") that browses/manages the recording library
		// and rebinds the record/replay hotkeys.
		class RecordingsTool : public FUCK::ITool
		{
		public:
			const char* Name() const override { return "Recordings"; }
			const char* Group() const override { return "devbench"; }

			// Feed input events to a hotkey being rebound. We NEVER call ProcessManagedHotkey, so a
			// managed hotkey never fires an action — firing stays the raw sink (zero double-fire).
			bool OnAsyncInput(const void* a_event) override
			{
				bool used = false;
				if (m_recordHK.isBinding)
					used |= FUCK::UpdateManagedHotkey(a_event, m_recordHK);
				if (m_replayHK.isBinding)
					used |= FUCK::UpdateManagedHotkey(a_event, m_replayHK);
				return used;
			}

			void Draw() override
			{
				FUCK::TextWrapped(
					"Replay a captured trajectory against the current build. Replay is async; it "
					"restores the recorded scene first (force = run anyway on a mismatch).");
				FUCK::Spacing();
				if (FUCK::Button("Replay last"))
					ReplayPath("");
				FUCK::Separator();

				const json list = dvb::ListRecordingsCached();
				if (list.contains("error")) {
					FUCK::TextColored(kRed, "recordings unavailable: %s", list.value("error", std::string{}).c_str());
					return;
				}
				const std::string dir = list.value("dir", std::string{});

				RenderActionBar();
				RenderTable(list, dir);
				RenderKeybinds();
			}

		private:
			void RenderActionBar()
			{
				FUCK::InputText("##filter", m_filter, sizeof m_filter);
				FUCK::BeginDisabled(m_selected.empty());
				if (FUCK::Button("Validate selected"))
					for (const auto& f : m_selected)
						Validate(f, true);
				FUCK::SameLine();
				if (FUCK::Button("Unvalidate selected"))
					for (const auto& f : m_selected)
						Validate(f, false);
				FUCK::SameLine();
				if (FUCK::Button("Delete selected"))
					m_confirmDelete = true;
				FUCK::EndDisabled();
				if (m_confirmDelete && !m_selected.empty()) {
					FUCK::SameLine();
					FUCK::TextColored(kRed, "delete %d?", static_cast<int>(m_selected.size()));
					FUCK::SameLine();
					if (FUCK::Button("Yes##confdel")) {
						for (const auto& f : m_selected)
							Delete(f);
						m_selected.clear();
						m_confirmDelete = false;
					}
					FUCK::SameLine();
					if (FUCK::Button("No##confdel"))
						m_confirmDelete = false;
				}
				FUCK::SameLine();
				if (FUCK::Button("Open folder"))
					dvb::OpenRecordingsFolder();
			}

			// Manual sortable header — FUCK exposes no TableGetSortSpecs, so a clicked header cell
			// toggles our own sort state that feeds ui::BuildRows.
			void HeaderCell(int a_col, const char* a_title, ui::SortKey a_key)
			{
				FUCK::TableNextColumn();
				std::string label = a_title;
				if (m_sortKey == a_key)
					label += m_sortAsc ? " ^" : " v";
				if (FUCK::Selectable((label + "##h" + std::to_string(a_col)).c_str())) {
					if (m_sortKey == a_key)
						m_sortAsc = !m_sortAsc;
					else {
						m_sortKey = a_key;
						m_sortAsc = true;
					}
				}
			}

			void RenderTable(const json& a_list, const std::string& a_dir)
			{
				const FUCK::TableFlags flags = FUCK::TableFlags::kResizable | FUCK::TableFlags::kRowBg |
				                               FUCK::TableFlags::kBordersInnerH;
				if (!FUCK::BeginTable("recs", 8, flags))
					return;
				FUCK::TableSetupColumn("##sel");
				FUCK::TableSetupColumn("Name");
				FUCK::TableSetupColumn("Where");
				FUCK::TableSetupColumn("Start");
				FUCK::TableSetupColumn("Time");
				FUCK::TableSetupColumn("Fmt");
				FUCK::TableSetupColumn("Val");
				FUCK::TableSetupColumn("Actions");

				FUCK::TableNextRow();
				FUCK::TableNextColumn();  // select column: no sort
				HeaderCell(1, "Name", ui::SortKey::Name);
				HeaderCell(2, "Where", ui::SortKey::Where);
				HeaderCell(3, "Start", ui::SortKey::Start);
				HeaderCell(4, "Time", ui::SortKey::Time);
				HeaderCell(5, "Fmt", ui::SortKey::Format);
				HeaderCell(6, "Val", ui::SortKey::Validated);
				FUCK::TableNextColumn();  // actions column: no sort

				for (const auto& row : ui::BuildRows(a_list, m_filter, m_sortKey, m_sortAsc)) {
					FUCK::TableNextRow();
					FUCK::TableNextColumn();
					bool sel = m_selected.count(row.file) > 0;
					if (FUCK::Checkbox(("##sel" + row.file).c_str(), &sel)) {
						if (sel)
							m_selected.insert(row.file);
						else
							m_selected.erase(row.file);
					}
					FUCK::TableNextColumn();
					FUCK::Text("%s", row.name.c_str());
					FUCK::TableNextColumn();
					FUCK::Text("%s", row.where.c_str());
					FUCK::TableNextColumn();
					if (row.restorable)
						FUCK::Text("%s", row.startText.c_str());
					else
						FUCK::TextColored(kRed, "%s", row.startText.c_str());
					FUCK::TableNextColumn();
					FUCK::Text("%s", row.timeText.c_str());
					FUCK::TableNextColumn();
					FUCK::Text("%s", row.format.c_str());
					FUCK::TableNextColumn();
					FUCK::TextColored(row.validated ? kGreen : kGrey, row.validated ? "yes" : "no");
					FUCK::TableNextColumn();
					const std::string id = "##" + row.file;
					if (FUCK::Button(("Replay" + id).c_str()))
						ReplayPath(a_dir + "/" + row.file);
					FUCK::SameLine();
					if (FUCK::Button(((row.validated ? "Unval" : "Val") + id).c_str()))
						Validate(row.file, !row.validated);
					FUCK::SameLine();
					if (FUCK::Button(("Del" + id).c_str())) {
						Delete(row.file);
						m_selected.erase(row.file);
					}
				}
				FUCK::EndTable();
			}

			void RenderKeybinds()
			{
				if (!FUCK::CollapsingHeader("Keybinds"))
					return;
				if (!m_seeded) {
					int  recKey = 0, repKey = 0;
					bool recShift = false, repShift = false;
					dvb::GetHotkeys(recKey, recShift, repKey, repShift);
					m_recordHK.kKey = static_cast<std::uint32_t>(recKey);
					m_recordHK.kMod1 = recShift ? static_cast<int>(FUCK::Modifier::kShift) : -1;
					m_replayHK.kKey = static_cast<std::uint32_t>(repKey);
					m_replayHK.kMod1 = repShift ? static_cast<int>(FUCK::Modifier::kShift) : -1;
					m_lastRecordKey = m_recordHK.kKey;
					m_lastReplayKey = m_replayHK.kKey;
					m_seeded = true;
				}
				FUCK::DrawManagedHotkey("Record", m_recordHK);
				ApplyBind(m_recordHK, m_lastRecordKey, true, m_recordWarn);
				if (!m_recordWarn.empty())
					FUCK::TextColored(kRed, "%s", m_recordWarn.c_str());
				FUCK::DrawManagedHotkey("Replay", m_replayHK);
				ApplyBind(m_replayHK, m_lastReplayKey, false, m_replayWarn);
				if (!m_replayWarn.empty())
					FUCK::TextColored(kRed, "%s", m_replayWarn.c_str());
			}

			char                  m_filter[128]{};
			std::set<std::string> m_selected;
			bool                  m_confirmDelete = false;
			ui::SortKey           m_sortKey = ui::SortKey::Name;
			bool                  m_sortAsc = true;
			FUCK::ManagedHotkey   m_recordHK, m_replayHK;
			std::uint32_t         m_lastRecordKey = 0, m_lastReplayKey = 0;
			bool                  m_seeded = false;
			std::string           m_recordWarn, m_replayWarn;
		};

		// Registered by pointer with FUCK; must outlive registration → static storage.
		RecordingsTool g_recordingsTool;
	}

	void RegisterFuck()
	{
		if (!FUCK::Connect("devbench")) {
			logger::info("FUCK not installed; devbench FUCK menu disabled.");
			return;
		}
		FUCK::RegisterTool(&g_recordingsTool);
		logger::info("devbench: registered FUCK recordings tool.");
	}
}
