#include "Tools.h"

#include "ConsoleLogCapture.h"
#include "EventBus.h"
#include "GameEvents.h"
#include "Json.h"
#include "MainThread.h"
#include "ToolRegistry.h"
#include "Version.h"

#include <chrono>
#include <filesystem>
#include <thread>

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

			if (action == "list") {
				json open = json::array();
				bool messageBox = false;
				for (const auto& m : GetOpenMenus()) {
					if (m == RE::MessageBoxMenu::MENU_NAME)
						messageBox = true;
					open.push_back(m);
				}
				return json{ { "openMenus", std::move(open) }, { "messageBoxOpen", messageBox } };
			}

			if (action == "close") {
				// Dismiss a menu by name via the UI message queue (kHide). For a modal
				// MessageBoxMenu this cancels/closes it — unblocking automated flows (e.g.
				// new-game popups). Marshalled to the main thread.
				const std::string name = a_args.value("name", std::string{});
				if (name.empty())
					throw ToolError(400, "action 'close' requires a 'name' (menu to hide)");
				auto* task = SKSE::GetTaskInterface();
				if (!task)
					throw ToolError(500, "SKSE TaskInterface unavailable");
				task->AddTask([name]() {
					if (auto* q = RE::UIMessageQueue::GetSingleton())
						q->AddMessage(name.c_str(), RE::UI_MESSAGE_TYPE::kHide, nullptr);
				});
				return json{ { "queued", true }, { "action", "close" }, { "name", name } };
			}

			throw ToolError(400, std::format("unknown action '{}' (list|close)", action));
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

		// ---- scenario: server-side timed replay of a step list -------------------
		// Runs on the listener thread, so it may sleep/poll directly and marshal each
		// action to the main thread. Steps are one of: { "tool", "args" } (dispatch a
		// registered tool — so any tool, incl. consumer-registered ones, is replayable),
		// { "wait": ms } (fixed pacing), { "waitFor": ... } (block on a Skyrim EVENT),
		// or { "waitUntil": cond } (poll live state). Prefer waitFor over wait: it keys
		// off the real signal (e.g. a load is done when lifecycle:postLoadGame fires).

		using namespace std::chrono;

		bool IsLifecycleName(const std::string& a_s)
		{
			return a_s == "dataLoaded" || a_s == "newGame" || a_s == "preLoadGame" ||
			       a_s == "postLoadGame" || a_s == "saveGame" || a_s == "deleteGame";
		}

		// Every key/value in `a_match` present and equal in `a_payload` (subset match).
		bool PayloadMatches(const json& a_payload, const json& a_match)
		{
			if (!a_match.is_object())
				return true;
			for (const auto& [k, v] : a_match.items()) {
				if (!a_payload.contains(k) || a_payload[k] != v)
					return false;
			}
			return true;
		}

		struct WaitForSpec
		{
			std::string topic;
			json match;
		};

		// Normalize a step's "waitFor" into {topic, match}. String shorthands: a
		// lifecycle event name → {lifecycle,{event}}; "menuOpened"/"menuClosed" (with the
		// step's "name") → {menu,{name,opening}}. An object is {topic, match} verbatim.
		WaitForSpec ParseWaitFor(const json& a_step)
		{
			const json& wf = a_step["waitFor"];
			WaitForSpec spec;
			if (wf.is_string()) {
				const std::string s = wf.get<std::string>();
				if (IsLifecycleName(s)) {
					spec.topic = "lifecycle";
					spec.match = json{ { "event", s } };
				} else if (s == "menuOpened" || s == "menuClosed") {
					spec.topic = "menu";
					spec.match = json{ { "name", a_step.value("name", std::string{}) }, { "opening", s == "menuOpened" } };
				} else {
					throw ToolError(400, std::format("unknown waitFor shorthand '{}'", s));
				}
			} else if (wf.is_object()) {
				spec.topic = wf.value("topic", std::string{});
				spec.match = wf.value("match", json::object());
				if (spec.topic.empty())
					throw ToolError(400, "waitFor object requires a 'topic'");
			} else {
				throw ToolError(400, "waitFor must be a string shorthand or {topic, match}");
			}
			return spec;
		}

		// Live-state conditions for waitUntil. playerLoaded marshals to the main thread;
		// a mid-load stall (RunAndWait 504) just means "not yet" → keep polling. Menu
		// conditions read the thread-safe tracked set (no marshal).
		bool CheckState(const std::string& a_cond)
		{
			if (a_cond == "playerLoaded") {
				try {
					const json r = MainThread::RunAndWait([]() -> json {
						auto* pc = RE::PlayerCharacter::GetSingleton();
						return pc && pc->Get3D() != nullptr;
					},
						milliseconds(2000));
					return r.get<bool>();
				} catch (const ToolError&) {
					return false;  // main thread stalled mid-load — condition not met yet
				}
			}
			if (a_cond == "noModal") {
				for (const auto& m : GetOpenMenus())
					if (m == RE::MessageBoxMenu::MENU_NAME)
						return false;
				return true;
			}
			if (a_cond == "noMenu")
				return GetOpenMenus().empty();
			throw ToolError(400, std::format("unknown waitUntil condition '{}' (playerLoaded|noModal|noMenu)", a_cond));
		}

		json ScenarioHandler(const json& a_args, const ToolContext& a_ctx,
			const ToolRegistry& a_registry, const EventBus& a_events)
		{
			if (!a_args.contains("steps") || !a_args["steps"].is_array())
				throw ToolError(400, "scenario requires a 'steps' array");
			const json& steps = a_args["steps"];

			int repeat = a_args.value("repeat", 1);
			if (repeat < 1)
				repeat = 1;
			if (repeat > 1000)
				throw ToolError(400, "repeat capped at 1000");
			const bool continueOnError = a_args.value("continueOnError", false);

			json results = json::array();
			const auto t0 = steady_clock::now();
			bool anyFailure = false;
			bool aborted = false;

			for (int rep = 0; rep < repeat && !aborted; ++rep) {
				for (size_t i = 0; i < steps.size() && !aborted; ++i) {
					const json& step = steps[i];
					json r{ { "index", i } };
					if (repeat > 1)
						r["repeat"] = rep;
					const auto stepStart = steady_clock::now();
					bool stepFailed = false;

					try {
						if (step.contains("wait")) {
							const long ms = step["wait"].get<long>();
							r["kind"] = "wait";
							r["ms"] = ms;
							std::this_thread::sleep_for(milliseconds(ms));
						} else if (step.contains("waitFor")) {
							const WaitForSpec spec = ParseWaitFor(step);
							const long timeoutMs = step.value("timeoutMs", static_cast<long>(60000));
							const long pollMs = step.value("pollMs", static_cast<long>(100));
							r["kind"] = "waitFor";
							r["topic"] = spec.topic;
							r["match"] = spec.match;
							// Only events published after this step begins count.
							uint64_t since = a_events.HeadSeq();
							bool satisfied = false;
							const auto deadline = steady_clock::now() + milliseconds(timeoutMs);
							while (steady_clock::now() < deadline) {
								for (const auto& ev : a_events.Since(since)) {
									since = ev.seq;
									if (ev.topic == spec.topic && PayloadMatches(ev.payload, spec.match)) {
										satisfied = true;
										break;
									}
								}
								if (satisfied)
									break;
								std::this_thread::sleep_for(milliseconds(pollMs));
							}
							r["satisfied"] = satisfied;
							if (!satisfied) {
								r["timedOut"] = true;
								stepFailed = true;
							}
						} else if (step.contains("waitUntil")) {
							const std::string cond = step["waitUntil"].get<std::string>();
							const long timeoutMs = step.value("timeoutMs", static_cast<long>(30000));
							const long pollMs = step.value("pollMs", static_cast<long>(250));
							r["kind"] = "waitUntil";
							r["cond"] = cond;
							bool satisfied = false;
							const auto deadline = steady_clock::now() + milliseconds(timeoutMs);
							do {
								if (CheckState(cond)) {
									satisfied = true;
									break;
								}
								std::this_thread::sleep_for(milliseconds(pollMs));
							} while (steady_clock::now() < deadline);
							r["satisfied"] = satisfied;
							if (!satisfied) {
								r["timedOut"] = true;
								stepFailed = true;
							}
						} else if (step.contains("tool")) {
							const std::string tool = step["tool"].get<std::string>();
							const json args = step.value("args", json::object());
							r["kind"] = "tool";
							r["tool"] = tool;
							if (step.contains("label"))
								r["label"] = step["label"];
							const ToolResult tr = a_registry.Invoke(tool, args, a_ctx);
							r["ok"] = tr.ok;
							if (tr.ok) {
								r["result"] = tr.value;
							} else {
								r["errorCode"] = tr.errorCode;
								r["error"] = tr.errorMessage;
								stepFailed = true;
							}
						} else {
							throw ToolError(400, std::format("step {} has none of wait/waitFor/waitUntil/tool", i));
						}
					} catch (const ToolError& e) {
						if (!r.contains("kind"))
							r["kind"] = "error";
						r["ok"] = false;
						r["errorCode"] = e.code;
						r["error"] = e.what();
						stepFailed = true;
					}

					r["elapsedMs"] = duration_cast<milliseconds>(steady_clock::now() - stepStart).count();
					results.push_back(std::move(r));

					if (stepFailed) {
						anyFailure = true;
						if (!continueOnError)
							aborted = true;
					}
				}
			}

			return json{
				{ "ok", !anyFailure },
				{ "aborted", aborted },
				{ "stepsRun", results.size() },
				{ "elapsedMs", duration_cast<milliseconds>(steady_clock::now() - t0).count() },
				{ "results", std::move(results) },
			};
		}
	}

	void RegisterCoreTools(ToolRegistry& a_registry, EventBus& a_events)
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
			"Inspect/dismiss menus. action='list' returns { openMenus, messageBoxOpen } "
			"tracked live from menu open/close events; action='close' hides a menu by "
			"'name' via the UI message queue (kHide) — for a MessageBoxMenu this cancels "
			"the blocking modal, unblocking automated new-game flows. (Reading a message "
			"box's text/buttons is still planned.)";
		menu.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
									 { "action", json{ { "type", "string" }, { "enum", json::array({ "list", "close" }) }, { "description", "list → { openMenus, messageBoxOpen }; close → hide a menu" } } },
									 { "name", json{ { "type", "string" }, { "description", "menu name to hide (required for close), e.g. MessageBoxMenu" } } },
								 } },
		};
		a_registry.Register(std::move(menu), &MenuHandler);

		ToolDescriptor scenario;
		scenario.name = "scenario";
		scenario.description =
			"Run a timed sequence of steps server-side (reproducible tests/benchmarks) and "
			"return a per-step transcript. Each step is one of: "
			"{\"tool\":\"<name>\",\"args\":{…}} dispatch any registered tool (e.g. console, game); "
			"{\"wait\":<ms>} fixed pacing; "
			"{\"waitFor\":<event>,…} block on a Skyrim EVENT — string shorthand "
			"(\"postLoadGame\"/\"saveGame\"/\"newGame\"/\"preLoadGame\"/\"dataLoaded\"/\"deleteGame\", or "
			"\"menuOpened\"/\"menuClosed\" with a \"name\"), or {\"topic\":\"…\",\"match\":{…}}; "
			"{\"waitUntil\":\"playerLoaded\"|\"noModal\"|\"noMenu\"} poll live state. "
			"PREFER waitFor over a fixed wait — e.g. wait for postLoadGame to know a load truly "
			"finished. Optional top-level: repeat (≤1000), continueOnError. waitFor/waitUntil take "
			"timeoutMs + pollMs. Blocks the request for the run's duration (seconds); keep "
			"per-step timeouts sane.";
		scenario.inputSchema = json{
			{ "type", "object" },
			{ "required", json::array({ "steps" }) },
			{ "properties", json{
									 { "steps", json{ { "type", "array" }, { "description", "ordered steps; each is one of tool/wait/waitFor/waitUntil (see description)" }, { "items", json{ { "type", "object" } } } } },
									 { "repeat", json{ { "type", "integer" }, { "description", "run the whole step list N times (default 1, max 1000)" } } },
									 { "continueOnError", json{ { "type", "boolean" }, { "description", "keep going after a failed/timed-out step instead of aborting (default false)" } } },
								 } },
		};
		a_registry.Register(std::move(scenario),
			[&a_registry, &a_events](const json& a_args, const ToolContext& a_ctx) {
				return ScenarioHandler(a_args, a_ctx, a_registry, a_events);
			});
	}
}
