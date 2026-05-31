#include "Tools.h"

#include "ConsoleLogCapture.h"
#include "GameEvents.h"
#include "Json.h"
#include "MainThread.h"
#include "ToolRegistry.h"
#include "Version.h"

#include <filesystem>

namespace dvb
{
	namespace
	{
		bool Truthy(const json& a_v)
		{
			if (a_v.is_boolean())
				return a_v.get<bool>();
			if (a_v.is_string()) {
				const auto s = a_v.get<std::string>();
				return s == "true" || s == "1";
			}
			if (a_v.is_number())
				return a_v.get<double>() != 0.0;
			return false;
		}

		// console: run a Skyrim console command; optionally fence + capture its output.
		json ConsoleHandler(const json& a_args, const ToolContext&)
		{
			const std::string action = a_args.value("action", std::string("exec"));

			if (action == "read") {
				const auto lines = ConsoleLogCapture::Snapshot(200);
				const bool markersFound = ConsoleLogCapture::SawBegin() && ConsoleLogCapture::SawEnd();
				json arr = json::array();
				for (const auto& l : lines)
					arr.push_back(json{ { "seq", l.seq }, { "frame", l.frame }, { "text", l.text } });
				json out{
					{ "markersFound", markersFound },
					{ "sawBegin", ConsoleLogCapture::SawBegin() },
					{ "sawEnd", ConsoleLogCapture::SawEnd() },
					{ "count", arr.size() },
					{ "headSeq", ConsoleLogCapture::HeadSeq() },
					{ "lines", std::move(arr) },
				};
				ConsoleLogCapture::EndCapture();  // close the window opened by exec(capture)
				return out;
			}
			if (action != "exec")
				throw ToolError(400, std::format("unknown action '{}'", action));

			const std::string command = a_args.value("command", std::string{});
			if (command.empty())
				throw ToolError(400, "missing required parameter 'command'");

			auto* task = SKSE::GetTaskInterface();
			if (!task)
				throw ToolError(500, "SKSE TaskInterface unavailable");

			const bool capture = a_args.contains("capture") && Truthy(a_args["capture"]);
			if (capture)
				ConsoleLogCapture::BeginCapture();

			// ExecuteCommand is deferred (GFx console drains queued commands on a later
			// tick). Fence the real command between two invalid marker commands; the
			// detour records only what lands between their echoed tokens. Capture
			// `command` by value so it outlives this lambda.
			task->AddTask([command, capture]() {
				if (capture)
					RE::Console::ExecuteCommand(ConsoleLogCapture::kMarkerBegin);
				RE::Console::ExecuteCommand(command.c_str());
				if (capture)
					RE::Console::ExecuteCommand(ConsoleLogCapture::kMarkerEnd);
			});

			return json{ { "queued", true }, { "command", command }, { "capturing", capture } };
		}

		// game: programmatic save / load via BGSSaveLoadManager. loadLast gives a settled
		// real-save state for testing WITHOUT coc's heavy new-game init. Runs on the main
		// thread (save/load mutate game state and queue async internally). Fire-and-forget
		// — watch lifecycle events / inspect playerLoaded for completion.
		json GameHandler(const json& a_args, const ToolContext&)
		{
			const std::string action = a_args.value("action", std::string{});

			if (action == "list") {
				// No console command lists saves and CommonLib doesn't expose the game's
				// save-entry array, so enumerate the saves directory ourselves (.ess stem =
				// the name `load` takes). Resolve the real dir rather than guessing:
				//   - explicit `dir` arg wins (escape hatch for exotic setups);
				//   - else <My Games game folder> / sLocalSavePath (read from the INI; the
				//     's' prefix guarantees it's a string), supporting an absolute setting.
				// Running in-process means MO2/USVFS-virtualized saves resolve correctly.
				// Caveat: bUseMyGamesDirectory=0 (saves under the install dir) isn't handled
				// — pass `dir` for that. The resolved dir is returned for transparency.
				namespace fs = std::filesystem;
				fs::path saveDir;
				if (const std::string dirArg = a_args.value("dir", std::string{}); !dirArg.empty()) {
					saveDir = dirArg;
				} else {
					const auto logDir = SKSE::log::log_directory();
					if (!logDir)
						throw ToolError(500, "save directory unavailable (no log dir)");
					fs::path local = "Saves";  // default sLocalSavePath
					if (auto* ini = RE::INISettingCollection::GetSingleton()) {
						if (auto* s = ini->GetSetting("sLocalSavePath:General")) {
							if (const char* sp = s->GetString(); sp && *sp)
								local = sp;
						}
					}
					saveDir = local.is_absolute() ? local : (logDir->parent_path() / local);
				}
				json saves = json::array();
				std::error_code ec;
				for (const auto& entry : fs::directory_iterator(saveDir, ec)) {
					if (entry.path().extension() == ".ess")
						saves.push_back(entry.path().stem().string());
				}
				return json{ { "dir", saveDir.string() }, { "count", saves.size() }, { "saves", std::move(saves) } };
			}

			auto* task = SKSE::GetTaskInterface();
			if (!task)
				throw ToolError(500, "SKSE TaskInterface unavailable");

			if (action == "loadLast") {
				task->AddTask([]() {
					if (auto* m = RE::BGSSaveLoadManager::GetSingleton())
						m->LoadMostRecentSaveGame();
				});
				return json{ { "queued", true }, { "action", action } };
			}
			if (action == "save" || action == "load") {
				const std::string name = a_args.value("name", std::string{});
				if (name.empty())
					throw ToolError(400, std::format("action '{}' requires a 'name'", action));
				const bool isSave = (action == "save");
				task->AddTask([name, isSave]() {
					auto* m = RE::BGSSaveLoadManager::GetSingleton();
					if (!m)
						return;
					if (isSave)
						m->Save(name.c_str());
					else
						// checkForMods=false: skip the "content no longer present" mismatch
						// confirmation modal so an automated load doesn't strand on it.
						m->Load(name.c_str(), false);
				});
				return json{ { "queued", true }, { "action", action }, { "name", name } };
			}
			throw ToolError(400, std::format("unknown action '{}' (save|load|loadLast)", action));
		}

		// menu: detect open menus / blocking modals, tracked live from MenuOpenCloseEvent
		// (no UI access on the listener thread). Reading a message box's text/buttons and
		// accepting/closing it are planned — they need the live MessageBoxData (CommonLib
		// exposes bodyText/buttonText but not an accessor to the active instance) + runtime
		// iteration. Detection already lets an agent know a modal is blocking and which.
		json MenuHandler(const json& a_args, const ToolContext&)
		{
			const std::string action = a_args.value("action", std::string("list"));
			if (action != "list")
				throw ToolError(400, std::format("unknown action '{}' (list)", action));
			json open = json::array();
			bool messageBox = false;
			for (const auto& m : GetOpenMenus()) {
				if (m == RE::MessageBoxMenu::MENU_NAME)
					messageBox = true;
				open.push_back(m);
			}
			return json{ { "openMenus", std::move(open) }, { "messageBoxOpen", messageBox } };
		}

		// inspect: read live game state. Demonstrates the value-returning primitive —
		// the read runs on the main thread and its result is returned synchronously.
		json InspectHandler(const json& a_args, const ToolContext&)
		{
			const std::string kind = a_args.value("kind", std::string("state"));
			if (kind != "state")
				throw ToolError(400, std::format("unknown kind '{}'", kind));

			return MainThread::RunAndWait([]() -> json {
				auto* pc = RE::PlayerCharacter::GetSingleton();
				const bool loaded = pc && pc->Get3D() != nullptr;
				return json{
					{ "plugin", "devbench" },
					{ "version", DEVBENCH_VERSION_STRING },
					{ "vr", REL::Module::IsVR() },
					{ "playerLoaded", loaded },
				};
			});
		}
	}

	void RegisterCoreTools(ToolRegistry& a_registry)
	{
		ToolDescriptor console;
		console.name = "console";
		console.description =
			"Run a Skyrim console command. Fire-and-forget by default (queued onto the "
			"main thread, runs next tick). With capture=true the command is fenced between "
			"marker commands so action='read' returns ONLY its output (markersFound=true), "
			"surviving heavy ConsoleLog spam. Useful for printing commands (getav, getgs, help).";
		console.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
									 { "action", json{ { "type", "string" }, { "enum", json::array({ "exec", "read" }) }, { "description", "'exec' (default) runs `command`; 'read' returns the fenced output and closes the window" } } },
									 { "command", json{ { "type", "string" }, { "description", "the console command, exactly as typed after ~ (required for exec)" } } },
									 { "capture", json{ { "type", "boolean" }, { "description", "exec: fence and capture this command's output for the next read" } } },
								 } },
		};
		a_registry.Register(std::move(console), &ConsoleHandler);

		ToolDescriptor game;
		game.name = "game";
		game.description =
			"Save/load and list saves. action='list' enumerates the saves directory and "
			"returns save names (use one as 'name' for load); 'loadLast' loads the most recent "
			"save (a settled real-game state — avoids coc's heavy new-game init); 'load'/'save' "
			"take a 'name' ('load' skips the mod-mismatch confirmation modal). All but 'list' "
			"are fire-and-forget; watch lifecycle events / inspect playerLoaded for completion.";
		game.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
									 { "action", json{ { "type", "string" }, { "enum", json::array({ "list", "save", "load", "loadLast" }) }, { "description", "list | save | load | loadLast" } } },
									 { "name", json{ { "type", "string" }, { "description", "save file name (required for save/load; from action='list')" } } },
									 { "dir", json{ { "type", "string" }, { "description", "list only: override the saves directory (default resolves from sLocalSavePath)" } } },
								 } },
		};
		a_registry.Register(std::move(game), &GameHandler);

		ToolDescriptor inspect;
		inspect.name = "inspect";
		inspect.description =
			"Read live game/plugin state. Runs on the main thread and returns the value "
			"synchronously (times out if the game is mid-load / not pumping tasks).";
		inspect.readOnly = true;
		inspect.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
									 { "kind", json{ { "type", "string" }, { "enum", json::array({ "state" }) }, { "description", "'state' → { plugin, version, vr, playerLoaded }" } } },
								 } },
		};
		a_registry.Register(std::move(inspect), &InspectHandler);

		ToolDescriptor menu;
		menu.name = "menu";
		menu.description =
			"Detect open menus / blocking modals. action='list' returns { openMenus, "
			"messageBoxOpen } tracked live from menu open/close events. (Reading a message "
			"box's text/buttons and accept/close are planned.)";
		menu.readOnly = true;
		menu.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
									 { "action", json{ { "type", "string" }, { "enum", json::array({ "list" }) }, { "description", "list → { openMenus, messageBoxOpen }" } } },
								 } },
		};
		a_registry.Register(std::move(menu), &MenuHandler);
	}
}
