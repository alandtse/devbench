// devbench's optional in-game menu, hosted by FUCK. PCH-FREE and must stay so: FUCK_API.h pulls in
// the real imgui.h, which can't coexist with SMF's cimgui ImGuiMCP — hence a separate PCH-free
// target (devbench-UI-fuck). Drawing routes through FUCK:: wrappers; inert when FUCK is absent
// (FUCK::Connect returns false).

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
#include <vector>

namespace logger = SKSE::log;

namespace dvb::UI
{
	namespace
	{
		const ImVec4 kGrey{ 0.7f, 0.7f, 0.7f, 1.0f };
		const ImVec4 kGreen{ 0.40f, 0.85f, 0.45f, 1.0f };
		const ImVec4 kRed{ 1.0f, 0.45f, 0.45f, 1.0f };

		// FUCK stores a bind's modifier in kMod1 as a raw keyboard DXScanCode (its KB_MODS), NOT the
		// Modifier enum. devbench's sink only honors Shift, so recognize left/right Shift by scancode.
		constexpr int kLeftShiftDX = 0x2A, kRightShiftDX = 0x36;
		bool          IsShiftMod(std::int32_t a_mod) { return a_mod == kLeftShiftDX || a_mod == kRightShiftDX; }

		void Tooltip(const char* a_text)
		{
			if (FUCK::IsItemHovered(0))
				FUCK::SetTooltip(a_text);
		}

		// Apply a completed rebind (called when UpdateManagedHotkey signals a captured key). Only Shift
		// and keyboard binds are supported, so a Ctrl/Alt or gamepad bind is rejected with a warning.
		void ApplyBind(FUCK::ManagedHotkey& a_h, bool a_isRecord, std::string& a_warn)
		{
			if (a_h.kKey == 0) {
				a_warn = "keyboard only — bind ignored";
				return;
			}
			if (a_h.kMod2 != -1 || (a_h.kMod1 != -1 && !IsShiftMod(a_h.kMod1))) {
				a_warn = "only Shift modifier supported";
				return;
			}
			a_warn.clear();
			const bool shift = IsShiftMod(a_h.kMod1);
			if (a_isRecord)
				dvb::SetRecordHotkey(static_cast<int>(a_h.kKey), shift);
			else
				dvb::SetReplayHotkey(static_cast<int>(a_h.kKey), shift);
			logger::info("devbench: rebound {} hotkey to key {} (shift={})", a_isRecord ? "record" : "replay", a_h.kKey, shift);
		}

		// A FUCK sidebar tool (grouped under "devbench") that browses/manages the recording library
		// and rebinds the record/replay hotkeys.
		class RecordingsTool : public FUCK::ITool
		{
		public:
			const char* Name() const override { return "Recordings"; }
			const char* Group() const override { return "devbench"; }

			// Feed every input event to the managed hotkeys (unconditional, mirroring FUCK's own
			// SettingsTool): UpdateManagedHotkey captures only while one is binding and returns true when
			// a bind completes — then apply it. We NEVER call ProcessManagedHotkey, so nothing fires here
			// (firing stays the raw sink; zero double-fire).
			bool OnAsyncInput(const void* a_event) override
			{
				bool consumed = false;
				if (FUCK::UpdateManagedHotkey(a_event, m_recordHK)) {
					ApplyBind(m_recordHK, true, m_recordWarn);
					consumed = true;
				}
				if (FUCK::UpdateManagedHotkey(a_event, m_replayHK)) {
					ApplyBind(m_replayHK, false, m_replayWarn);
					consumed = true;
				}
				return consumed;
			}

			// Abort any in-progress rebind when the menu closes, so a hotkey isn't left stuck binding.
			void OnClose() override
			{
				FUCK::AbortManagedHotkey(m_recordHK);
				FUCK::AbortManagedHotkey(m_replayHK);
			}

			void Draw() override
			{
				RenderHotkeys();  // always visible at the top — rebinding is the least discoverable feature
				FUCK::Separator();

				if (FUCK::Button("Replay most recent"))
					ReplayAction("");
				Tooltip("Replay the most recently recorded trajectory against the current build (async).");
				RenderStatus();
				FUCK::Separator();

				const json list = dvb::ListRecordingsCached();
				if (list.contains("error")) {
					FUCK::TextColored(kRed, "recordings unavailable: %s", list.value("error", std::string{}).c_str());
					return;
				}
				const std::string dir = list.value("dir", std::string{});

				FUCK::Text("Filter");
				FUCK::SameLine();
				FUCK::InputText("##filter", m_filter, sizeof m_filter);

				const auto rows = ui::BuildRows(list, m_filter, m_sortKey, m_sortAsc);
				RenderBulkBar(rows);

				if (rows.empty()) {
					RenderEmptyState();
					return;
				}
				RenderTable(rows, dir);
			}

		private:
			// --- action wrappers: drive the shared helper, then set the status/error line ---
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
			void SetStatus(const std::string& a_s)
			{
				m_lastStatus = a_s;
				m_lastError.clear();
			}
			void SetError(const std::string& a_e)
			{
				m_lastError = a_e;
				m_lastStatus.clear();
			}
			void RenderStatus()
			{
				if (!m_lastError.empty())
					FUCK::TextColored(kRed, "%s", m_lastError.c_str());
				else if (!m_lastStatus.empty())
					FUCK::TextColored(kGreen, "%s", m_lastStatus.c_str());
			}

			void RenderHotkeys()
			{
				if (!m_seeded) {
					int  recKey = 0, repKey = 0;
					bool recShift = false, repShift = false;
					dvb::GetHotkeys(recKey, recShift, repKey, repShift);
					m_recordHK.kKey = static_cast<std::uint32_t>(recKey);
					m_recordHK.kMod1 = recShift ? kLeftShiftDX : -1;
					m_replayHK.kKey = static_cast<std::uint32_t>(repKey);
					m_replayHK.kMod1 = repShift ? kLeftShiftDX : -1;
					m_seeded = true;
				}
				// Click a widget then press a key to rebind; capture + apply happen in OnAsyncInput.
				FUCK::Text("Hotkeys (click, then press a key to rebind):");
				FUCK::DrawManagedHotkey("Record", m_recordHK);
				if (!m_recordWarn.empty())
					FUCK::TextColored(kRed, "%s", m_recordWarn.c_str());
				FUCK::DrawManagedHotkey("Replay", m_replayHK);
				if (!m_replayWarn.empty())
					FUCK::TextColored(kRed, "%s", m_replayWarn.c_str());
			}

			void RenderEmptyState()
			{
				if (m_filter[0]) {
					FUCK::TextColored(kGrey, "No matches for \"%s\"", m_filter);
					return;
				}
				int  recKey = 0, repKey = 0;
				bool recShift = false, repShift = false;
				dvb::GetHotkeys(recKey, recShift, repKey, repShift);
				FUCK::TextColored(kGrey, "No recordings yet — press %s to record.", ui::KeyName(recKey).c_str());
			}

			// Select-all + bulk actions over the currently-filtered rows.
			void RenderBulkBar(const std::vector<ui::Row>& a_rows)
			{
				if (FUCK::Button("All"))
					for (const auto& r : a_rows)
						m_selected.insert(r.file);
				FUCK::SameLine();
				if (FUCK::Button("None"))
					m_selected.clear();
				FUCK::SameLine();
				FUCK::Text("(%d selected)", static_cast<int>(m_selected.size()));

				FUCK::BeginDisabled(m_selected.empty());
				FUCK::SameLine();
				if (FUCK::Button("Validate selected"))
					BulkValidate(true);
				FUCK::SameLine();
				if (FUCK::Button("Unvalidate selected"))
					BulkValidate(false);
				FUCK::SameLine();
				if (FUCK::Button("Delete selected"))
					m_confirmBulkDelete = true;
				FUCK::EndDisabled();
				if (m_confirmBulkDelete && !m_selected.empty()) {
					FUCK::SameLine();
					FUCK::TextColored(kRed, "delete %d?", static_cast<int>(m_selected.size()));
					FUCK::SameLine();
					if (FUCK::Button("Yes##confdel")) {
						BulkDelete();
						m_confirmBulkDelete = false;
					}
					FUCK::SameLine();
					if (FUCK::Button("No##confdel"))
						m_confirmBulkDelete = false;
				}
				FUCK::SameLine();
				if (FUCK::Button("Open folder"))
					dvb::OpenRecordingsFolder();
			}

			void BulkValidate(bool a_value)
			{
				int         fail = 0;
				std::string first;
				for (const auto& f : m_selected)
					if (const std::string e = ui::Validate(f, a_value, false); !e.empty()) {
						++fail;
						if (first.empty())
							first = e;
					}
				dvb::InvalidateRecordingsCache();
				ReportBulk(fail, static_cast<int>(m_selected.size()), a_value ? "validated" : "unvalidated", first);
			}
			void BulkDelete()
			{
				int         fail = 0;
				std::string first;
				for (const auto& f : m_selected)
					if (const std::string e = ui::Delete(f, false); !e.empty()) {
						++fail;
						if (first.empty())
							first = e;
					}
				dvb::InvalidateRecordingsCache();
				ReportBulk(fail, static_cast<int>(m_selected.size()), "deleted", first);
				m_selected.clear();
			}
			void ReportBulk(int a_fail, int a_total, const char* a_verb, const std::string& a_first)
			{
				if (a_fail == 0)
					SetStatus(std::to_string(a_total) + " " + a_verb);
				else
					SetError(std::to_string(a_fail) + " of " + std::to_string(a_total) + " " + a_verb + " failed: " + a_first);
			}

			// Manual sortable header — FUCK exposes no TableGetSortSpecs. A trailing marker shows the
			// active sort (^/v) and that the others are clickable (-).
			void HeaderCell(int a_col, const char* a_title, ui::SortKey a_key)
			{
				FUCK::TableNextColumn();
				std::string label = a_title;
				label += m_sortKey == a_key ? (m_sortAsc ? " ^" : " v") : " -";
				if (FUCK::Selectable((label + "##h" + std::to_string(a_col)).c_str())) {
					if (m_sortKey == a_key)
						m_sortAsc = !m_sortAsc;
					else {
						m_sortKey = a_key;
						m_sortAsc = true;
					}
				}
			}

			void RenderTable(const std::vector<ui::Row>& a_rows, const std::string& a_dir)
			{
				FUCK::TextColored(kGrey, "click a column header to sort");
				const FUCK::TableFlags flags = FUCK::TableFlags::kResizable | FUCK::TableFlags::kRowBg |
				                               FUCK::TableFlags::kBordersInnerH;
				if (!FUCK::BeginTable("recs", 7, flags))
					return;
				FUCK::TableSetupColumn("Sel");
				FUCK::TableSetupColumn("Name");
				FUCK::TableSetupColumn("Where");
				FUCK::TableSetupColumn("Start");
				FUCK::TableSetupColumn("Time");
				FUCK::TableSetupColumn("Valid");
				FUCK::TableSetupColumn("Actions");

				FUCK::TableNextRow();
				FUCK::TableNextColumn();  // select column: no sort
				FUCK::Text("Sel");
				HeaderCell(1, "Name", ui::SortKey::Name);
				HeaderCell(2, "Where", ui::SortKey::Where);
				HeaderCell(3, "Start", ui::SortKey::Start);
				HeaderCell(4, "Time", ui::SortKey::Time);
				HeaderCell(6, "Valid", ui::SortKey::Validated);
				FUCK::TableNextColumn();  // actions column: no sort

				for (const auto& row : a_rows) {
					FUCK::TableNextRow();
					FUCK::TableNextColumn();
					bool sel = m_selected.count(row.file) > 0;
					if (FUCK::Checkbox(("##sel" + row.file).c_str(), &sel, false, false)) {
						if (sel)
							m_selected.insert(row.file);
						else
							m_selected.erase(row.file);
					}
					FUCK::TableNextColumn();
					FUCK::Text("%s", row.name.c_str());
					Tooltip(("format: " + row.format).c_str());
					FUCK::TableNextColumn();
					FUCK::Text("%s", row.where.c_str());
					FUCK::TableNextColumn();
					if (row.restorable)
						FUCK::Text("%s", row.startText.c_str());
					else {
						FUCK::TextColored(kRed, "%s", row.startText.c_str());
						Tooltip("No save/coc entry captured — replay can't restore the starting scene; comparisons may be unreliable.");
					}
					FUCK::TableNextColumn();
					FUCK::Text("%s", row.timeText.c_str());
					FUCK::TableNextColumn();
					FUCK::TextColored(row.validated ? kGreen : kGrey, row.validated ? "yes" : "no");
					FUCK::TableNextColumn();
					RenderRowActions(row, a_dir);
				}
				FUCK::EndTable();
			}

			void RenderRowActions(const ui::Row& a_row, const std::string& a_dir)
			{
				const std::string id = "##" + a_row.file;
				if (FUCK::Button(("Replay" + id).c_str()))
					ReplayAction(a_dir + "/" + a_row.file);
				Tooltip("Replay this recording.");
				FUCK::SameLine();
				if (FUCK::Button(((a_row.validated ? "Unvalidate" : "Validate") + id).c_str()))
					ValidateAction(a_row.file, !a_row.validated);
				Tooltip("Mark this recording validated / not.");
				FUCK::SameLine();
				// Per-row delete needs a confirm too (bulk delete already does).
				if (m_confirmDeleteFile == a_row.file) {
					if (FUCK::Button(("Yes" + id).c_str())) {
						DeleteAction(a_row.file);
						m_selected.erase(a_row.file);
						m_confirmDeleteFile.clear();
					}
					FUCK::SameLine();
					if (FUCK::Button(("No" + id).c_str()))
						m_confirmDeleteFile.clear();
				} else {
					if (FUCK::Button(("Delete" + id).c_str()))
						m_confirmDeleteFile = a_row.file;
					Tooltip("Delete this recording (asks to confirm).");
				}
			}

			char                  m_filter[128]{};
			std::set<std::string> m_selected;
			bool                  m_confirmBulkDelete = false;
			std::string           m_confirmDeleteFile;  // the row awaiting a delete confirm
			ui::SortKey           m_sortKey = ui::SortKey::Name;
			bool                  m_sortAsc = true;
			FUCK::ManagedHotkey   m_recordHK, m_replayHK;
			bool                  m_seeded = false;
			std::string           m_recordWarn, m_replayWarn;
			std::string           m_lastError, m_lastStatus;  // last action outcome (red / green)
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
