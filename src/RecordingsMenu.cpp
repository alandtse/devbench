// devbench's optional in-game menu, hosted by SKSE Menu Framework 3.
//
// This TU is PCH-FREE and must stay that way: the SMF client header exposes cimgui-style typedefs
// (ImGuiMCP) that cannot coexist with the real imgui.h the main PCH pulls in. All drawing goes
// through ImGuiMCP (the HOST's ImGui context, resolved via GetProcAddress) — never a local imgui.
// Because SMF resolves every call at runtime, the whole module is inert when the framework DLL is
// absent (SKSEMenuFramework::IsInstalled() gates registration).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "RecordingsMenu.h"

#include <RE/Skyrim.h>  // RE::InputEvent — referenced by SMF header callback typedefs
#include <SKSE/SKSE.h>  // SKSE::log (this TU has no PCH `logs` alias)

#include "SKSEMenuFramework.h"

#include "Json.h"    // dvb::json (nlohmann) — unrelated to imgui, safe in this TU
#include "Server.h"  // dvb::RunTool

#include <fstream>
#include <string>

namespace logger = SKSE::log;

namespace dvb::UI
{
	namespace
	{
		constexpr ImGuiMCP::ImVec4 kGrey{ 0.7f, 0.7f, 0.7f, 1.0f };
		constexpr ImGuiMCP::ImVec4 kGreen{ 0.40f, 0.85f, 0.45f, 1.0f };
		constexpr ImGuiMCP::ImVec4 kRed{ 1.0f, 0.45f, 0.45f, 1.0f };

		// Replay a recording (or, with an empty path, the most recent). ASYNC by default so the
		// call returns immediately — a blocking replay here would stall the render thread.
		void ReplayPath(const std::string& a_path)
		{
			json args{ { "action", "replay" }, { "restoreScene", true }, { "force", true } };
			if (!a_path.empty())
				args["path"] = a_path;
			dvb::RunTool("record", args);
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
			const json        recs = list.value("recordings", json::array());
			ImGuiMCP::Text("%d recording(s):", static_cast<int>(recs.size()));
			ImGuiMCP::Spacing();

			int idx = 0;
			for (const auto& r : recs) {
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

				const std::string path = dir + "/" + file;
				if (ImGuiMCP::Button((std::string("Replay") + id).c_str()))
					ReplayPath(path);
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button((std::string(validated ? "Unvalidate" : "Validate") + id).c_str())) {
					dvb::RunTool("recordings", json{ { "action", "validate" }, { "file", file }, { "value", !validated } });
					dvb::InvalidateRecordingsCache();
				}
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button((std::string("Delete") + id).c_str())) {
					dvb::RunTool("recordings", json{ { "action", "delete" }, { "file", file } });
					dvb::InvalidateRecordingsCache();
				}
				ImGuiMCP::Separator();
			}
		}

		void __stdcall RenderKeybinds()
		{
			ImGuiMCP::TextWrapped(
				"Record/replay hotkeys are set in Data/SKSE/Plugins/devbench/config.json "
				"(DXScanCode integers; 0 = disabled). In-menu rebinding is a planned follow-up.");
			ImGuiMCP::Separator();
			json cfg;
			try {
				if (std::ifstream in("Data/SKSE/Plugins/devbench/config.json"); in)
					in >> cfg;
			} catch (...) {
			}
			ImGuiMCP::Text("recordHotkey : %d", cfg.value("recordHotkey", 0));
			ImGuiMCP::Text("replayHotkey : %d", cfg.value("replayHotkey", 0));
			const std::string rp = cfg.value("replayPath", std::string{});
			ImGuiMCP::Text("replayPath   : %s", rp.empty() ? "(most recent)" : rp.c_str());
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
