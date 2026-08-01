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

#include "Json.h"    // dvb::json (nlohmann) — unrelated to imgui, safe in this TU
#include "Server.h"  // dvb::RunTool

#include <fstream>
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

		// A FUCK sidebar tool (grouped under "devbench") that browses/manages the recording library.
		class RecordingsTool : public FUCK::ITool
		{
		public:
			const char* Name() const override { return "Recordings"; }
			const char* Group() const override { return "devbench"; }

			void Draw() override
			{
				FUCK::TextWrapped(
					"Replay a captured trajectory against the current build. Replay is async; it "
					"restores the recorded scene first (force = run anyway on a mismatch).");
				FUCK::Spacing();
				if (FUCK::Button("Replay last"))
					ReplayPath("");
				FUCK::Separator();

				const json list = dvb::RunTool("recordings", json{ { "action", "list" } });
				if (list.contains("error")) {
					FUCK::TextColored(kRed, "recordings unavailable: %s", list.value("error", std::string{}).c_str());
					return;
				}
				const std::string dir = list.value("dir", std::string{});
				const json        recs = list.value("recordings", json::array());
				FUCK::Text("%d recording(s):", static_cast<int>(recs.size()));
				FUCK::Spacing();

				int idx = 0;
				for (const auto& r : recs) {
					const std::string file = r.value("file", std::string{});
					const std::string name = r.value("name", file);
					const std::string fmt = r.value("format", std::string{ "?" });
					const bool        validated = r.value("validated", false);
					const int         samples = r.value("sampleCount", 0);
					const std::string id = "##rec" + std::to_string(idx++);

					FUCK::Text("%s", name.c_str());
					FUCK::TextColored(kGrey, "  %s | %d samples", fmt.c_str(), samples);
					FUCK::SameLine();
					FUCK::TextColored(validated ? kGreen : kGrey, validated ? "| validated" : "| unvalidated");

					const std::string path = dir + "/" + file;
					if (FUCK::Button((std::string("Replay") + id).c_str()))
						ReplayPath(path);
					FUCK::SameLine();
					if (FUCK::Button((std::string(validated ? "Unvalidate" : "Validate") + id).c_str()))
						dvb::RunTool("recordings", json{ { "action", "validate" }, { "file", file }, { "value", !validated } });
					FUCK::SameLine();
					if (FUCK::Button((std::string("Delete") + id).c_str()))
						dvb::RunTool("recordings", json{ { "action", "delete" }, { "file", file } });
					FUCK::Separator();
				}
			}
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
